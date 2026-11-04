#include "mqtt_manager.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "uart_handler.h"
#include "packet_manager.h"
#include "ota_mqtt.h"

#define TAG "MQTTMGR"

esp_mqtt_client_handle_t mqtt_client = NULL;

#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>

#define MQTT_NVS_NAMESPACE "mqtt"
#define MQTT_NVS_KEY_MAC "central_mac"
#define MQTT_TOPIC_MAX_LEN 128
#define MAC_STR_LEN 18 // "aa:bb:cc:dd:ee:ff" + null

static char g_central_mac[MAC_STR_LEN] = {0};
static bool g_mac_loaded_from_nvs = false;
static char g_mqtt_sub_topic[MQTT_TOPIC_MAX_LEN];
static char g_mqtt_pub_topic[MQTT_TOPIC_MAX_LEN];

static void build_topics_from_mac(const char *mac)
{
    snprintf(g_mqtt_sub_topic, sizeof(g_mqtt_sub_topic), "/%s/wifi/receive", mac);
    snprintf(g_mqtt_pub_topic, sizeof(g_mqtt_pub_topic), "/%s/wifi/send", mac);
}

static void mqtt_topics_load(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(MQTT_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_OK)
    {
        size_t req = 0;
        if (nvs_get_str(h, MQTT_NVS_KEY_MAC, NULL, &req) == ESP_OK && req <= MAC_STR_LEN)
        {
            nvs_get_str(h, MQTT_NVS_KEY_MAC, g_central_mac, &req);
            g_mac_loaded_from_nvs = true;
        }
        nvs_close(h);
    }

    g_central_mac[MAC_STR_LEN - 1] = '\0';

    if (g_mac_loaded_from_nvs)
    {
        build_topics_from_mac(g_central_mac);
        ESP_LOGI(TAG, "Loaded central MAC from NVS: '%s' -> sub: '%s', pub: '%s'",
                 g_central_mac, g_mqtt_sub_topic, g_mqtt_pub_topic);
    }
    else
    {
        ESP_LOGW(TAG, "No central MAC in NVS, MQTT topics not configured. Waiting for CMD_40.");
    }
}

bool mqtt_manager_set_central_mac(const char *mac)
{
    if (!mac || strlen(mac) == 0 || strlen(mac) >= MAC_STR_LEN)
        return false;

    nvs_handle_t h;
    esp_err_t err = nvs_open(MQTT_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK)
        return false;

    err = nvs_set_str(h, MQTT_NVS_KEY_MAC, mac);
    if (err == ESP_OK)
        err = nvs_commit(h);
    nvs_close(h);

    if (err == ESP_OK)
    {
        strncpy(g_central_mac, mac, MAC_STR_LEN);
        g_central_mac[MAC_STR_LEN - 1] = '\0';
        build_topics_from_mac(g_central_mac);
        ESP_LOGI(TAG, "Central MAC set: '%s' -> sub: '%s', pub: '%s'",
                 g_central_mac, g_mqtt_sub_topic, g_mqtt_pub_topic);

        // Re-subscribe if MQTT is already connected
        if (mqtt_client)
        {
            esp_mqtt_client_subscribe(mqtt_client, g_mqtt_sub_topic, 1);
        }
        return true;
    }
    return false;
}

const char *mqtt_manager_get_central_mac(void)
{
    return g_central_mac;
}

bool mqtt_manager_has_central_mac(void)
{
    return g_mac_loaded_from_nvs;
}

void mqtt_manager_load_mac(void)
{
    mqtt_topics_load();
}

static bool s_connected = false;

// --- Netif failover state ---
static esp_netif_t *s_wifi_netif = NULL;
static esp_netif_t *s_eth_netif = NULL;
static int64_t s_last_swap_time_us = 0;
#define SWAP_COOLDOWN_MS 5000 // don't swap more than once per x seconds

void netif_failover_init(esp_netif_t *wifi_netif, esp_netif_t *eth_netif)
{
    s_wifi_netif = wifi_netif;
    s_eth_netif = eth_netif;
    ESP_LOGI(TAG, "Netif failover initialized (wifi=%p, eth=%p)", wifi_netif, eth_netif);
}

static void swap_default_netif(void)
{
    // Cooldown: avoid flapping between interfaces
    int64_t now_us = esp_timer_get_time();
    if ((now_us - s_last_swap_time_us) < (SWAP_COOLDOWN_MS * 1000LL))
    {
        ESP_LOGD(TAG, "Netif swap cooldown active, skipping");
        return;
    }

    // Determine which interface to try next
    // We track preference with a simple toggle
    esp_netif_t *candidate = NULL;
    static bool s_prefer_wifi = true; // start preferring wifi

    if (s_prefer_wifi)
    {
        candidate = s_eth_netif;
    }
    else
    {
        candidate = s_wifi_netif;
    }

    if (candidate == NULL)
    {
        ESP_LOGW(TAG, "No alternate netif available for failover");
        return;
    }

    // Only swap if the candidate interface has a valid IP (is actually connected)
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(candidate, &ip_info) != ESP_OK || ip_info.ip.addr == 0)
    {
        ESP_LOGW(TAG, "Candidate netif has no IP, cannot swap");
        return;
    }

    // Perform the swap by setting the candidate as default
    esp_netif_set_default_netif(candidate);
    s_prefer_wifi = !s_prefer_wifi;
    s_last_swap_time_us = now_us;

    ESP_LOGI(TAG, "Default netif swapped to %s (ip=" IPSTR ")",
             candidate == s_eth_netif ? "Ethernet" : "WiFi",
             IP2STR(&ip_info.ip));
}

// copy mqtt_event_handler_cb and mqtt_app_start implementation
static void mqtt_event_handler_cb(void *handler_args, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    uint8_t packet[] = {0xAA, 0xAA, 0x00, 0x01, PKT_TYPE_CONFIG, PKT_CMD_41, 0x00, 0x00, 0xBB, 0xBB};
    switch (event->event_id)
    {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        // subscribe to the topic loaded from NVS (or default)
        esp_mqtt_client_subscribe(event->client, g_mqtt_sub_topic, 1);

        // Subscribe to OTA command topic
        ota_mqtt_init();

        if (!s_connected)
        {
            packet[6] = 0x01;                // Online status in data field
            packet[7] = crc8(&packet[0], 7); // Calculate CRC for header, length, packet_type and cmd
            uart_handler_write_bytes(packet, sizeof(packet));
            ESP_LOGI(TAG, "CMD 41 - Status Online 0x01 sent to UART");
        }
        s_connected = true;
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        // Try switching to the other network interface for the next reconnect attempt
        swap_default_netif();
        if (s_connected)
        {
            packet[6] = 0x00;                // Online status in data field
            packet[7] = crc8(&packet[0], 7); // Calculate CRC for header, length, packet_type and cmd
            uart_handler_write_bytes(packet, sizeof(packet));
            ESP_LOGI(TAG, "CMD 41 - Status Offline 0x00 sent to UART");
        }
        s_connected = false;
        break;
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "MQTT_EVENT_DATA");

        // Check if this is an OTA command
        if (ota_mqtt_handle_message(event->topic, event->topic_len,
                                    event->data, event->data_len)) {
            break; // Consumed by OTA handler
        }

        uint8_t *heap_buf = pvPortMalloc(event->data_len);
        if (heap_buf)
        {
            memcpy(heap_buf, event->data, event->data_len);
            packet_t pkt = {.source = PACKET_SOURCE_MQTT, .data = heap_buf, .length = event->data_len, .packet_type = heap_buf[4]};
            if (!packet_manager_submit(&pkt, 0))
                vPortFree(heap_buf);
        }
        else
        {
            ESP_LOGE(TAG, "MQTT_EVENT_DATA: malloc failed (%d bytes), inbound message dropped", event->data_len);
        }
        break;
    default:
        break;
    }
    return;
}

// Amazon Root CA 1
static const char ca_cert_pem[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIDQTCCAimgAwIBAgITBmyfz5m/jAo54vB4ikPmljZbyjANBgkqhkiG9w0BAQsF\n"
    "ADA5MQswCQYDVQQGEwJVUzEPMA0GA1UEChMGQW1hem9uMRkwFwYDVQQDExBBbWF6\n"
    "b24gUm9vdCBDQSAxMB4XDTE1MDUyNjAwMDAwMFoXDTM4MDExNzAwMDAwMFowOTEL\n"
    "MAkGA1UEBhMCVVMxDzANBgNVBAoTBkFtYXpvbjEZMBcGA1UEAxMQQW1hem9uIFJv\n"
    "b3QgQ0EgMTCCASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBALJ4gHHKeNXj\n"
    "ca9HgFB0fW7Y14h29Jlo91ghYPl0hAEvrAIthtOgQ3pOsqTQNroBvo3bSMgHFzZM\n"
    "9O6II8c+6zf1tRn4SWiw3te5djgdYZ6k/oI2peVKVuRF4fn9tBb6dNqcmzU5L/qw\n"
    "IFAGbHrQgLKm+a/sRxmPUDgH3KKHOVj4utWp+UhnMJbulHheb4mjUcAwhmahRWa6\n"
    "VOujw5H5SNz/0egwLX0tdHA114gk957EWW67c4cX8jJGKLhD+rcdqsq08p8kDi1L\n"
    "93FcXmn/6pUCyziKrlA4b9v7LWIbxcceVOF34GfID5yHI9Y/QCB/IIDEgEw+OyQm\n"
    "jgSubJrIqg0CAwEAAaNCMEAwDwYDVR0TAQH/BAUwAwEB/zAOBgNVHQ8BAf8EBAMC\n"
    "AYYwHQYDVR0OBBYEFIQYzIU07LwMlJQuCFmcx7IQTgoIMA0GCSqGSIb3DQEBCwUA\n"
    "A4IBAQCY8jdaQZChGsV2USggNiMOruYou6r4lK5IpDB/G/wkjUu0yKGX9rbxenDI\n"
    "U5PMCCjjmCXPI6T53iHTfIUJrU6adTrCC2qJeHZERxhlbI1Bjjt/msv0tadQ1wUs\n"
    "N+gDS63pYaACbvXy8MWy7Vu33PqUXHeeE6V/Uq2V8viTO96LXFvKWlJbYK8U90vv\n"
    "o/ufQJVtMVT8QtPHRh8jrdkPSHCa2XV4cdFyQzR1bldZwgJcJmApzyMZFo6IQ6XU\n"
    "5MsI+yMRQ+hDKXJioaldXgjUkK642M4UwtBV8ob2xJNDd2ZhwLnoQdeXeGADbkpy\n"
    "rqXRfboQnoZsG4q5WTP468SQvvG5\n"
    "-----END CERTIFICATE-----\n";

void mqtt_app_start(void)
{
    // Load topics from NVS (or use defaults)
    mqtt_topics_load();

    esp_mqtt_client_config_t mqtt_cfg = {
        // .broker.address.uri = "mqtt://test.mosquitto.org:1883", // Use your broker's URI and port
        .broker.address.uri = "mqtts://d05598481v60psohm9ifo-ats.iot.us-east-1.amazonaws.com:8883", // Use your broker's URI and port
        .network.timeout_ms = 15000,                                                                // 15s per blocking net op (DNS/TLS/connect/read/write); < 30s task WDT
        .network.reconnect_timeout_ms = 15000,                                                      // 15s between retries
        .session.keepalive = 180,                                                                   // 3 mins
        .credentials.username = "ppaAlarmHdw",
        .credentials.authentication.password = "tHWEP3RbZVw97deY",
        .broker.verification.certificate = (const char *)ca_cert_pem, // Uncomment if using a custom CA cert
        // .client.certificate = ..., // Optional: client certificate for mutual auth
        // .client.key = ...,  // Optional: client key for mutual auth
        // .broker.verification.use_global_ca_store = true,
        // .broker.verification.skip_cert_common_name_check = true, // Set true only for testing/self-signed
    };

    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, MQTT_EVENT_ANY, mqtt_event_handler_cb, NULL);
    esp_mqtt_client_start(mqtt_client);
}

int mqtt_manager_publish(const char *topic, const char *data, int len, int qos, int retain)
{
    if (mqtt_client == NULL)
    {
        ESP_LOGE(TAG, "MQTT client not initialized");
        return -1;
    }
    int msg_id = esp_mqtt_client_publish(mqtt_client, topic, data, len, qos, retain);
    if (msg_id == -1)
    {
        ESP_LOGE(TAG, "Failed to publish message to topic: %s", topic);
    }
    else
    {
        ESP_LOGI(TAG, "Published message to topic: %s, msg_id=%d", topic, msg_id);
    }
    return msg_id;
}

int mqtt_manager_publish_default(const char *data, int len, int qos, int retain)
{
    return mqtt_manager_publish(g_mqtt_pub_topic, data, len, qos, retain);
}

bool mqtt_manager_online(void)
{
    return mqtt_client != NULL;
}

bool mqtt_manager_connected(void)
{
    return s_connected;
}