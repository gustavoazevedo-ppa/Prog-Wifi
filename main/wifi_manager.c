#include "wifi_manager.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "nvs.h"

#define WIFI_SSID "PPA-MINAS"
#define WIFI_PASS "ppalegero"
#define WIFI_ENABLED 1 // 1 = Wi-Fi on, 0 = Wi-Fi disabled (driver never started, Ethernet only)
#define WIFI_MAX_RETRY 1
#define WIFI_RECONNECT_DELAY_MS 5000 // Base delay for the first delayed reconnect attempt

// Exponential backoff bounds for periodic reconnect. A permanently-bad AP would
// otherwise churn Wi-Fi/DMA buffers every 5s forever, starving the ENC28J60 driver
// (see "no mem for receive buffer" / "tx_ready_sem expired"). Backoff caps the churn.
#define WIFI_RECONNECT_MAX_DELAY_MS 300000 // 5 minutes ceiling
#define WIFI_RECONNECT_BACKOFF_FACTOR 2    // double the delay after each failed attempt

#define MAX_SSID_LEN 32
#define MAX_PASS_LEN 64

static bool s_wifi_started = false;
static bool s_wifi_init = false;
static esp_netif_t *s_wifi_netif = NULL;

EventGroupHandle_t s_wifi_event_group;

static unsigned int s_retry_num = 0;
static esp_timer_handle_t s_wifi_reconnect_timer = NULL;
static uint32_t s_reconnect_delay_ms = WIFI_RECONNECT_DELAY_MS; // current backoff delay

#define TAG "WIFIMGR"

// Log DMA-capable heap headroom. The ENC28J60 RX path allocates receive buffers
// from MALLOC_CAP_DMA; when that pool is exhausted the driver logs "no mem for
// receive buffer". Sampling it here lets us correlate reconnect churn with the
// memory pressure that breaks Ethernet. This is read-only and harmless.
static void wifi_log_dma_heap(const char *when)
{
    size_t dma_free = heap_caps_get_free_size(MALLOC_CAP_DMA);
    size_t dma_largest = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
    size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "heap[%s] DMA free=%u largest=%u | internal free=%u",
             when, (unsigned)dma_free, (unsigned)dma_largest, (unsigned)internal_free);
}

// Timer callback - one-shot; attempts a reconnect, then reschedules itself with an
// exponentially increasing delay (capped) until a connection is established.
static void wifi_reconnect_timer_callback(void *arg)
{
    ESP_LOGI(TAG, "Timer: attempting WiFi reconnect (attempt #%d, next backoff %u ms)",
             s_retry_num + 1, (unsigned)s_reconnect_delay_ms);
    wifi_log_dma_heap("reconnect");
    esp_wifi_connect();
    s_retry_num++;

    // Grow the delay for the next attempt, capped at the ceiling.
    uint32_t next_delay = s_reconnect_delay_ms * WIFI_RECONNECT_BACKOFF_FACTOR;
    if (next_delay > WIFI_RECONNECT_MAX_DELAY_MS || next_delay < s_reconnect_delay_ms)
    {
        next_delay = WIFI_RECONNECT_MAX_DELAY_MS;
    }
    s_reconnect_delay_ms = next_delay;

    // Re-arm the one-shot timer with the (possibly larger) delay.
    if (s_wifi_reconnect_timer != NULL)
    {
        esp_timer_start_once(s_wifi_reconnect_timer, (uint64_t)s_reconnect_delay_ms * 1000);
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        if (s_retry_num < WIFI_MAX_RETRY)
        {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Immediate reconnect attempt %d/%d", s_retry_num, WIFI_MAX_RETRY);
        }
        else
        {
            // Max immediate retries reached - switch to delayed reconnects with
            // exponential backoff (one-shot timer that re-arms itself).
            if (s_wifi_reconnect_timer == NULL)
            {
                s_reconnect_delay_ms = WIFI_RECONNECT_DELAY_MS; // reset backoff on fresh failure
                esp_timer_create_args_t timer_args = {
                    .callback = wifi_reconnect_timer_callback,
                    .arg = NULL,
                    .name = "wifi_reconnect_timer"};
                ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_wifi_reconnect_timer));
                ESP_ERROR_CHECK(esp_timer_start_once(s_wifi_reconnect_timer, (uint64_t)s_reconnect_delay_ms * 1000));
                ESP_LOGI(TAG, "Max retries reached (%d). Starting reconnect with backoff (base %d ms, cap %d ms)",
                         WIFI_MAX_RETRY, WIFI_RECONNECT_DELAY_MS, WIFI_RECONNECT_MAX_DELAY_MS);
            }

            // Still signal WIFI_FAIL_BIT to allow wifi_init_sta() to return if waiting
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        // Stop the reconnect timer if it's running
        if (s_wifi_reconnect_timer != NULL)
        {
            esp_timer_stop(s_wifi_reconnect_timer);
            esp_timer_delete(s_wifi_reconnect_timer);
            s_wifi_reconnect_timer = NULL;
            ESP_LOGI(TAG, "Reconnect timer stopped - WiFi connected");
        }

        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        s_reconnect_delay_ms = WIFI_RECONNECT_DELAY_MS; // reset backoff for next disconnect

        // Get the DNS server address
        esp_netif_dns_info_t dns;
        const char *dns_types[] = {"Main", "Backup", "Fallback"};

        for (int i = 0; i < 3; ++i)
        {
            if (esp_netif_get_dns_info(event->esp_netif, (esp_netif_dns_type_t)i, &dns) == ESP_OK)
            {
                ESP_LOGI("NET", "%s DNS: " IPSTR, dns_types[i], IP2STR(&dns.ip.u_addr.ip4));
            }
        }

        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

bool wifi_is_enabled(void)
{
    return WIFI_ENABLED != 0;
}

void wifi_init_sta(void)
{
    if (!WIFI_ENABLED)
    {
        ESP_LOGW(TAG, "Wi-Fi disabled (WIFI_ENABLED=0): driver not started");
        return;
    }

    if (!s_wifi_init)
    {
        s_wifi_event_group = xEventGroupCreate();

        s_wifi_netif = esp_netif_create_default_wifi_sta();

        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));

        esp_event_handler_instance_t instance_any_id;
        esp_event_handler_instance_t instance_got_ip;
        ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                            ESP_EVENT_ANY_ID,
                                                            &wifi_event_handler,
                                                            NULL,
                                                            &instance_any_id));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                            IP_EVENT_STA_GOT_IP,
                                                            &wifi_event_handler,
                                                            NULL,
                                                            &instance_got_ip));
        s_wifi_init = true;
    }

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = "",
            .password = "",
            // Connection filters
            .threshold = {
                .rssi = -75,                    // Reject any AP with a signal weaker than -75dBm
                .authmode = WIFI_AUTH_WPA2_PSK, // Reject any AP not using WPA2 PSK
            },
            .pmf_cfg = {.capable = true, .required = false}}};

    char ssid[MAX_SSID_LEN + 1] = {0}, pass[MAX_PASS_LEN + 1] = {0};
    if (wifi_load_credentials(ssid, sizeof(ssid), pass, sizeof(pass)))
    {
        ESP_LOGI(TAG, "Loaded credentials from NVS: %s", ssid);
        strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
        strncpy((char *)wifi_config.sta.password, pass, sizeof(wifi_config.sta.password) - 1);
    }
    else
    {
        ESP_LOGI(TAG, "No saved credentials");
        // TODO: remove this???
        wifi_store_credentials(WIFI_SSID, WIFI_PASS);
        return;
    }

    s_wifi_started = true;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum number of re-tries (WIFI_FAIL_BIT).
       Note: After WIFI_FAIL_BIT is set, a background timer continues periodic reconnect attempts. */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT)
    {
        ESP_LOGI(TAG, "connected to ap SSID:%s", ssid);
    }
    else if (bits & WIFI_FAIL_BIT)
    {
        ESP_LOGI(TAG, "Initial connection retries exhausted (%d attempts). Background reconnect active with exponential backoff (base %d ms, cap %d ms)",
                 WIFI_MAX_RETRY, WIFI_RECONNECT_DELAY_MS, WIFI_RECONNECT_MAX_DELAY_MS);
    }
    else
    {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }
}

bool wifi_store_credentials(const char *ssid, const char *pass)
{
    // TODO: encrypt with AES before storing?
    if (strlen(ssid) > MAX_SSID_LEN || strlen(pass) > MAX_PASS_LEN)
    {
        ESP_LOGW(TAG, "SSID or password length exceeds maximum allowed");
        return false;
    }
    else if (strlen(ssid) == 0 || strlen(pass) < 8) // WPA2 minimum password length is 8
    {
        ESP_LOGW(TAG, "SSID cannot be empty and password must be at least 8 characters long");
        return false;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open("wifi", NVS_READWRITE, &h);
    if (err != ESP_OK)
        return false;
    err = nvs_set_str(h, "ssid", ssid);
    if (err == ESP_OK)
        err = nvs_set_str(h, "pass", pass);
    if (err == ESP_OK)
        err = nvs_commit(h);
    nvs_close(h);
    return (err == ESP_OK);
}

bool wifi_load_credentials(char *ssid_out, size_t ssid_out_len,
                           char *pass_out, size_t pass_out_len)
{
    // TODO: decrypt with AES after loading?
    nvs_handle_t h;
    esp_err_t err = nvs_open("wifi", NVS_READONLY, &h);
    if (err != ESP_OK)
        return false;

    size_t required = 0;
    err = nvs_get_str(h, "ssid", NULL, &required);
    if (err == ESP_OK && required <= ssid_out_len)
    {
        nvs_get_str(h, "ssid", ssid_out, &required);
    }
    else
    {
        nvs_close(h);
        return false;
    }

    required = 0;
    err = nvs_get_str(h, "pass", NULL, &required);
    if (err == ESP_OK && required <= pass_out_len)
    {
        nvs_get_str(h, "pass", pass_out, &required);
    }
    else
    {
        nvs_close(h);
        return false;
    }

    nvs_close(h);
    return true;
}

bool wifi_clear_credentials(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("wifi", NVS_READWRITE, &h);
    if (err != ESP_OK)
        return false;
    nvs_erase_key(h, "ssid");
    nvs_erase_key(h, "pass");
    nvs_commit(h);
    nvs_close(h);
    return true;
}

bool wifi_update_credentials(const char *ssid, const char *pass)
{
    char ssid_old[MAX_SSID_LEN + 1] = {0}, pass_old[MAX_PASS_LEN + 1] = {0};
    if (wifi_load_credentials(ssid_old, sizeof(ssid_old), pass_old, sizeof(pass_old)))
    {
        if (strcmp(ssid, ssid_old) == 0 && strcmp(pass, pass_old) == 0)
        {
            ESP_LOGI(TAG, "New credentials are the same as existing ones, no update needed");
        }
        else if (!wifi_store_credentials(ssid, pass))
        {
            wifi_clear_credentials();
            ESP_LOGE(TAG, "Clearing credentials");
            esp_wifi_disconnect();
            return false;
        }
    }
    else if (!wifi_store_credentials(ssid, pass))
    {
        wifi_clear_credentials();
        ESP_LOGE(TAG, "Clearing credentials");
        esp_wifi_disconnect();
        return false;
    }

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = "",
            .password = "",
            // Connection filters
            .threshold = {
                .rssi = -75,                    // Reject any AP with a signal weaker than -75dBm
                .authmode = WIFI_AUTH_WPA2_PSK, // Reject any AP not using WPA2 PSK
            },
            .pmf_cfg = {.capable = true, .required = false}}};

    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, pass, sizeof(wifi_config.sta.password) - 1);

    ESP_LOGI(TAG, "Updating WiFi credentials to SSID:%s, Password:%s", ssid, pass);

    if (s_wifi_started)
    {
        // New credentials: clear stale retry/backoff state so a failed first
        // attempt retries promptly instead of waiting out the accumulated backoff.
        // Stop any pending backoff callback to avoid it racing with this reconnect.
        s_retry_num = 0;
        s_reconnect_delay_ms = WIFI_RECONNECT_DELAY_MS;
        if (s_wifi_reconnect_timer != NULL)
        {
            esp_timer_stop(s_wifi_reconnect_timer);
        }
        esp_wifi_disconnect();
        esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
        esp_wifi_connect();
    }
    else
    {
        ESP_LOGI(TAG, "WiFi driver is not started");
        wifi_init_sta();
    }

    return true;
}

esp_netif_t *wifi_get_netif(void)
{
    return s_wifi_netif;
}
