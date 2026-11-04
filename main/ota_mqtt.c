/****************************************************************************
 * OTA MQTT Command Handler
 *
 * Subscribes to: cmd/ppa/<central_mac>/ota
 * Publishes to:  dt/ppa/<central_mac>/ota_status
 *
 * On receiving {"action":"start"}, triggers a remote OTA update.
 ****************************************************************************/

#include "ota_mqtt.h"
#include "ota_button.h"
#include "fw_slot_sender.h"
#include "mqtt_manager.h"
#include "device_restart.h"
#include "esp_log.h"
#include "esp_app_desc.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

#define TAG "OTA_MQTT"

#define TOPIC_MAX_LEN 80

static char s_ota_cmd_topic[TOPIC_MAX_LEN];
static char s_ota_status_topic[TOPIC_MAX_LEN];
static bool s_initialized = false;

/* --------------------------------------------------------------------------
 * Status publishing
 * -------------------------------------------------------------------------- */

static void publish_status(const char *status, const char *detail)
{
    char payload[192];
    const esp_app_desc_t *app = esp_app_get_description();

    if (detail) {
        snprintf(payload, sizeof(payload),
            "{\"status\":\"%s\",\"detail\":\"%s\",\"fw_ver\":\"%s\"}",
            status, detail, app->version);
    } else {
        snprintf(payload, sizeof(payload),
            "{\"status\":\"%s\",\"fw_ver\":\"%s\"}",
            status, app->version);
    }

    mqtt_manager_publish(s_ota_status_topic, payload, strlen(payload), 1, 0);
}

/* Status callback for the central-firmware slot sender. Forwards its terminal
 * states to the same ota_status topic via publish_status. */
static void central_fw_status_cb(const char *status, const char *detail)
{
    publish_status(status, detail);
}

/* --------------------------------------------------------------------------
 * OTA task — runs on a dedicated high-stack task
 * -------------------------------------------------------------------------- */

static void ota_mqtt_task(void *arg)
{
    const char *manifest_url = (const char *)arg;

    publish_status("started", NULL);

    /* Give MQTT enough time to deliver the status message before it gets
     * destroyed inside free_memory_for_ota(). QoS 1 needs broker ACK. */
    vTaskDelay(pdMS_TO_TICKS(1000));

    esp_err_t result = ota_start_remote(manifest_url);

    if (result == ESP_OK) {
        ESP_LOGI(TAG, "OTA success, rebooting to activate new firmware");
        device_restart("ota_success");
    } else if (result == ESP_ERR_NOT_FOUND) {
        ESP_LOGI(TAG, "Firmware is up to date, rebooting to restore services");
        device_restart("ota_no_update");
    } else {
        ESP_LOGE(TAG, "OTA failed: %s, rebooting to restore services",
                 esp_err_to_name(result));
        device_restart("ota_failed");
    }
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

void ota_mqtt_init(void)
{
    const char *mac = mqtt_manager_get_central_mac();
    if (mac[0] == '\0') return;

    snprintf(s_ota_cmd_topic, sizeof(s_ota_cmd_topic), "cmd/ppa/%s/ota", mac);
    snprintf(s_ota_status_topic, sizeof(s_ota_status_topic), "dt/ppa/%s/ota_status", mac);

    /* Subscribe to the OTA command topic */
    if (mqtt_client) {
        esp_mqtt_client_subscribe(mqtt_client, s_ota_cmd_topic, 1);
    }

    /* Route central-firmware relay terminal states to the ota_status topic. */
    fw_slot_sender_set_status_cb(central_fw_status_cb);

    s_initialized = true;
    ESP_LOGI(TAG, "OTA MQTT initialized, listening on: %s", s_ota_cmd_topic);
}

bool ota_mqtt_handle_message(const char *topic, int topic_len,
                             const char *data, int data_len)
{
    if (!s_initialized) return false;

    /* Check if this message is for our OTA command topic */
    if (topic_len != (int)strlen(s_ota_cmd_topic)) return false;
    if (strncmp(topic, s_ota_cmd_topic, topic_len) != 0) return false;

    /* Parse the command {"action": "start"} */
    cJSON *root = cJSON_ParseWithLength(data, data_len);
    if (!root) {
        ESP_LOGW(TAG, "Invalid JSON in OTA command");
        return true; /* Consumed but invalid */
    }

    cJSON *action = cJSON_GetObjectItem(root, "action");
    if (!cJSON_IsString(action)) {
        ESP_LOGW(TAG, "Missing or invalid 'action' field");
        cJSON_Delete(root);
        return true;
    }

    const char *manifest_url = NULL;

    if (strcmp(action->valuestring, "start") == 0) {
        manifest_url = NULL; /* production manifest */
    } else if (strcmp(action->valuestring, "beta") == 0) {
        manifest_url = OTA_MANIFEST_URL_BETA;
    } else if (strcmp(action->valuestring, "central") == 0) {
        /* Central-firmware update: download the encrypted file and relay it to
         * the alarm panel over UART. This is NOT an ESP32 self-OTA — it does
         * not reboot the module. Runs on its own task inside the slot sender. */
        cJSON_Delete(root);
        if (fw_slot_sender_busy()) {
            ESP_LOGW(TAG, "Central firmware update already in progress, ignoring");
            return true;
        }
        ESP_LOGI(TAG, "OTA command received (action=central), starting central firmware relay...");
        publish_status("central_started", NULL);
        fw_slot_sender_start();
        return true;
    } else {
        ESP_LOGW(TAG, "Unknown OTA action: %s", action->valuestring);
        cJSON_Delete(root);
        return true;
    }

    cJSON_Delete(root);

    /* Don't start if already in progress */
    if (ota_button_leds_busy()) {
        ESP_LOGW(TAG, "OTA already in progress, ignoring");
        return true;
    }

    /* Launch OTA on a separate task (needs large stack for TLS) */
    ESP_LOGI(TAG, "OTA command received (action=%s), starting update...",
             manifest_url ? "beta" : "start");
    xTaskCreate(ota_mqtt_task, "ota_mqtt", 8192, (void *)manifest_url, 5, NULL);

    return true;
}
