/****************************************************************************
 * Telemetry Module
 *
 * Publishes device identity on MQTT connect and periodic health reports.
 * Topics:
 *   dt/ppa/<central_mac>/identity  — once on connect
 *   dt/ppa/<central_mac>/health    — every TELEMETRY_INTERVAL_MS
 ****************************************************************************/

#include "telemetry.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "mqtt_manager.h"
#include "wifi_manager.h"
#include "monip_client.h"
#include "onvif_events.h"
#include "device_info.h"
#include "device_restart.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define TAG "TELEMETRY"

/* Publish interval in milliseconds (default 10 minutes) */
#define TELEMETRY_INTERVAL_MS   (10 * 60 * 1000)

/* Random jitter added to first publish to avoid thundering herd */
#define TELEMETRY_JITTER_MAX_MS (10 * 1000)

/* Max topic and payload sizes */
#define TOPIC_MAX_LEN   80
#define PAYLOAD_MAX_LEN 320

/* External state */
extern bool ethernet_connected;
extern EventGroupHandle_t s_wifi_event_group;

/* Cached restart info (read once from NVS, cleared after read) */
static char s_hw_rst_reason[12] = "";
static char s_sw_rst_reason[24] = "";

/* --------------------------------------------------------------------------
 * Helpers
 * -------------------------------------------------------------------------- */

static void get_wifi_mac_str(char *buf, size_t buf_len)
{
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    snprintf(buf, buf_len, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static int get_wifi_rssi(void)
{
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        return ap_info.rssi;
    }
    return 0;
}

static bool is_wifi_connected(void)
{
    if (s_wifi_event_group == NULL) return false;
    return (xEventGroupGetBits(s_wifi_event_group) & WIFI_CONNECTED_BIT) != 0;
}

static const char *get_default_interface(void)
{
    esp_netif_t *default_netif = esp_netif_get_default_netif();
    if (default_netif == NULL) return "none";

    const char *desc = esp_netif_get_desc(default_netif);
    if (desc && strstr(desc, "eth")) return "eth";
    if (desc && strstr(desc, "sta")) return "wifi";
    return "unknown";
}

/* --------------------------------------------------------------------------
 * Publish functions
 * -------------------------------------------------------------------------- */

static void publish_identity(void)
{
    const char *central_mac = mqtt_manager_get_central_mac();
    if (central_mac[0] == '\0') return; /* Not provisioned yet */

    const esp_app_desc_t *app = esp_app_get_description();
    char wifi_mac[18];
    get_wifi_mac_str(wifi_mac, sizeof(wifi_mac));

    /* Read restart reasons once (clears NVS key on first read) */
    if (s_hw_rst_reason[0] == '\0') {
        device_get_last_restart_info(s_hw_rst_reason, sizeof(s_hw_rst_reason),
                                     s_sw_rst_reason, sizeof(s_sw_rst_reason));
    }

    char topic[TOPIC_MAX_LEN];
    snprintf(topic, sizeof(topic), "dt/ppa/%s/identity", central_mac);

    char payload[PAYLOAD_MAX_LEN];
    snprintf(payload, sizeof(payload),
        "{\"fw_ver\":\"%s\",\"hw_ver\":%d,\"wifi_mac\":\"%s\","
        "\"central_mac\":\"%s\",\"hw_rst_reason\":\"%s\","
        "\"sw_rst_reason\":\"%s\"}",
        app->version,
        HW_VERSION,
        wifi_mac,
        central_mac,
        s_hw_rst_reason,
        s_sw_rst_reason);

    mqtt_manager_publish(topic, payload, strlen(payload), 1, 1);
}

static void publish_health(void)
{
    const char *central_mac = mqtt_manager_get_central_mac();
    if (central_mac[0] == '\0') return;

    char topic[TOPIC_MAX_LEN];
    snprintf(topic, sizeof(topic), "dt/ppa/%s/health", central_mac);

    int64_t uptime_s = esp_timer_get_time() / 1000000LL;

    char payload[PAYLOAD_MAX_LEN];
    snprintf(payload, sizeof(payload),
        "{\"uptime\":%lld,\"free_heap\":%lu,\"min_heap\":%lu,"
        "\"task_count\":%lu,"
        "\"wifi_rssi\":%d,\"wifi_connected\":%s,\"eth_connected\":%s,"
        "\"default_if\":\"%s\",\"monip_a\":%s,\"monip_b\":%s,"
        "\"active_cameras\":%u}",
        (long long)uptime_s,
        (unsigned long)esp_get_free_heap_size(),
        (unsigned long)esp_get_minimum_free_heap_size(),
        (unsigned long)uxTaskGetNumberOfTasks(),
        get_wifi_rssi(),
        is_wifi_connected() ? "true" : "false",
        ethernet_connected ? "true" : "false",
        get_default_interface(),
        monip_receiver_a_connected() ? "true" : "false",
        monip_receiver_b_connected() ? "true" : "false",
        (unsigned)onvif_events_active_camera_count());

    mqtt_manager_publish(topic, payload, strlen(payload), 0, 0);
}

/* --------------------------------------------------------------------------
 * Telemetry task
 * -------------------------------------------------------------------------- */

static void telemetry_task(void *arg)
{
    bool identity_sent = false;

    /* Random initial delay to spread fleet publishes after power events */
    uint32_t jitter_ms = (uint32_t)(esp_random() % TELEMETRY_JITTER_MAX_MS);
    vTaskDelay(pdMS_TO_TICKS(jitter_ms));

    while (1) {
        /* Short poll interval to detect MQTT connect quickly for identity */
        vTaskDelay(pdMS_TO_TICKS(identity_sent ? TELEMETRY_INTERVAL_MS : 5000));

        /* Only publish when MQTT is connected */
        if (!mqtt_manager_connected()) {
            // identity_sent = false; /* Re-send identity on next connect */
            continue;
        }

        /* Send identity once per connection, but only after stable uptime
         * to avoid flooding during crash loops */
        if (!identity_sent) {
            int64_t uptime_s = esp_timer_get_time() / 1000000LL;
            if (uptime_s < 60) continue; /* Wait until device is stable */
            publish_identity();
            identity_sent = true;
        } else {
            /* Periodic health */
            publish_health();
        }
    }
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

void telemetry_init(void)
{
    xTaskCreate(telemetry_task, "telemetry", 3072, NULL, 3, NULL);
}
