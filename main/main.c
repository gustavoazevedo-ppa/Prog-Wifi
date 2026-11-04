/****************************************************************************
 *
 *
 ****************************************************************************/

#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "driver/gpio.h"
#include "packet_manager.h"
#include "packet_actions.h"
#include "uart_handler.h"
#include "ble_gatt.h"
#include "ethernet_manager.h"
#include "wifi_manager.h"
#include "mqtt_manager.h"
#include "onvif_events.h"
#include "udp_broadcast.h"
#include "nvs_schema.h"
#include "monip_client.h"
#include "ota_button.h"
#include "esp_ota_ops.h"
#include "telemetry.h"

#define TAG "PPA"

#define WIFI_LED_PIN GPIO_NUM_33
#define MQTT_LED_PIN GPIO_NUM_32

extern EventGroupHandle_t s_wifi_event_group;

// ─── Boot health timer callback ─────────────────────────────────────────────
// If the device survives 30s without crashing, the boot is considered healthy.
static void boot_health_timer_cb(TimerHandle_t timer)
{
    nvs_boot_mark_healthy();
    xTimerDelete(timer, 0);
}

static void status_led_task(void *arg)
{
    while (1)
    {
        // Delay before next check
        vTaskDelay(pdMS_TO_TICKS(2000)); // Check every 2 seconds

        // WiFi LED always reflects WiFi status (OTA only uses MQTT LED)
        if (s_wifi_event_group != NULL)
        {
            EventBits_t wifi_bits = xEventGroupGetBits(s_wifi_event_group);
            bool wifi_connected = (wifi_bits & WIFI_CONNECTED_BIT) != 0;
            gpio_set_level(WIFI_LED_PIN, wifi_connected ? 0 : 1); // On if connected
        }
        else
        {
            gpio_set_level(WIFI_LED_PIN, 1); // Off if status unknown
        }

        // Skip MQTT LED update when OTA process owns it
        if (ota_button_leds_busy()) {
            continue;
        }

        // Check MQTT status
        bool mqtt_connected = mqtt_manager_connected();
        gpio_set_level(MQTT_LED_PIN, mqtt_connected ? 0 : 1); // On if connected
    }
}

void app_main(void)
{
    esp_err_t ret;

    // Initialize NVS.
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Validate NVS schema version — erases NVS if layout has changed
    nvs_schema_check();

    // Boot loop protection: if device panic-rebooted N times in a row,
    // erase NVS to break the loop (corrupted data is the likely cause)
    nvs_boot_loop_check();

    // Initialize TCP/IP network interface aka the esp-netif (should be called only once in application)
    ESP_ERROR_CHECK(esp_netif_init());

    // Create default event loop that running in background
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Initialize Packet Manager and register packet actions
    packet_manager_init();
    packet_actions_init();
    monip_init();

    uart_handler_init();

    // Initialize Ethernet early — DHCP needs time to complete before ONVIF/MQTT
    ethernet_init();

    // Configure status LEDs and blink to signal boot (visual restart indicator).
    // The 500ms delay doubles as settling time for Ethernet DHCP + ENC28J60 driver.
    gpio_reset_pin(WIFI_LED_PIN);
    gpio_set_direction(WIFI_LED_PIN, GPIO_MODE_OUTPUT);
    gpio_reset_pin(MQTT_LED_PIN);
    gpio_set_direction(MQTT_LED_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(WIFI_LED_PIN, 0); // On
    gpio_set_level(MQTT_LED_PIN, 0); // On
    vTaskDelay(pdMS_TO_TICKS(500));
    gpio_set_level(WIFI_LED_PIN, 1); // Off
    gpio_set_level(MQTT_LED_PIN, 1); // Off

    ble_gatt_init();

    // wifi_store_credentials("PED-TESTES", "74gu19u0Du"); 
    // wifi_store_credentials("PPA-MINAS", "ppalegero");
    // wifi_clear_credentials();
    wifi_init_sta();

    // Mark firmware as valid — rollback protection.
    // If we reach this point, basic init succeeded (NVS, netif, WiFi/Ethernet drivers).
    // If a bad OTA causes a crash before here, the bootloader will revert automatically.
    esp_ota_mark_app_valid_cancel_rollback();

    // Initialize netif failover: swap default interface on MQTT disconnect
    netif_failover_init(wifi_get_netif(), ethernet_get_netif());

    // Load central MAC from NVS early so we can start services that depend on it
    mqtt_manager_load_mac();

    // Start periodic UDP broadcast if central MAC is already known from NVS
    const char *central_mac = mqtt_manager_get_central_mac();
    if (central_mac[0] != '\0')
    {
        char discovery_msg[24];
        snprintf(discovery_msg, sizeof(discovery_msg), "PPA_%s\r\n", central_mac);
        udp_broadcast_start((const uint8_t *)discovery_msg, strlen(discovery_msg), 30303);
    }

    // Initialize OTA button handler early — gives user a physical recovery
    // path even if later init stages (ONVIF, MQTT) cause issues
    ota_button_init();

    // Load camera configs from NVS and start ONVIF polling tasks
    onvif_events_init();

    // Start telemetry reporting (identity on connect, health every 10 min)
    telemetry_init();

    // Start status LED monitoring task
    xTaskCreate(status_led_task, "status_led_task", 1024, NULL, 5, NULL);

    // Start 30s one-shot timer — if we survive this long, mark boot healthy
    // (resets the panic counter so normal operation isn't penalized)
    TimerHandle_t health_timer = xTimerCreate(
        "boot_health", pdMS_TO_TICKS(30000), pdFALSE, NULL, boot_health_timer_cb);
    if (health_timer != NULL)
    {
        xTimerStart(health_timer, 0);
    }

    return;
}