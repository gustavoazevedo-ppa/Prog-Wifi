/****************************************************************************
 * Scaffold: WiFi + Ethernet (independent) + OTA button + boot safety
 ****************************************************************************/

#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "driver/gpio.h"
#include "ethernet_manager.h"
#include "wifi_manager.h"
#include "nvs_schema.h"
#include "ota_button.h"
#include "esp_ota_ops.h"

#define TAG "APP"

#define WIFI_LED_PIN GPIO_NUM_33
#define ETH_LED_PIN GPIO_NUM_32

extern EventGroupHandle_t s_wifi_event_group;

// ─── Boot health timer callback ─────────────────────────────────────────────
// If the device survives 30s without crashing, the boot is considered healthy.
static void boot_health_timer_cb(TimerHandle_t timer)
{
    nvs_boot_mark_healthy();
    xTimerDelete(timer, 0);
}

static bool netif_has_ip(esp_netif_t *netif)
{
    esp_netif_ip_info_t info;
    if (netif == NULL || esp_netif_get_ip_info(netif, &info) != ESP_OK)
    {
        return false;
    }
    return info.ip.addr != 0;
}

static void status_led_task(void *arg)
{
    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(2000));

        // The OTA process owns the ETH LED while it runs (it blinks it)
        if (ota_button_leds_busy())
        {
            continue;
        }

        // LEDs are active-low (0 = on)
        bool wifi_connected = false;
        if (s_wifi_event_group != NULL)
        {
            wifi_connected = (xEventGroupGetBits(s_wifi_event_group) & WIFI_CONNECTED_BIT) != 0;
        }
        gpio_set_level(WIFI_LED_PIN, wifi_connected ? 0 : 1);

        gpio_set_level(ETH_LED_PIN, netif_has_ip(ethernet_get_netif()) ? 0 : 1);
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

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Initialize Ethernet early — DHCP needs time to complete
    ethernet_init();

    // Configure status LEDs and blink to signal boot (visual restart indicator).
    // The 500ms delay doubles as settling time for Ethernet DHCP + ENC28J60 driver.
    gpio_reset_pin(WIFI_LED_PIN);
    gpio_set_direction(WIFI_LED_PIN, GPIO_MODE_OUTPUT);
    gpio_reset_pin(ETH_LED_PIN);
    gpio_set_direction(ETH_LED_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(WIFI_LED_PIN, 0); // On
    gpio_set_level(ETH_LED_PIN, 0);  // On
    vTaskDelay(pdMS_TO_TICKS(500));
    gpio_set_level(WIFI_LED_PIN, 1); // Off
    gpio_set_level(ETH_LED_PIN, 1);  // Off

    wifi_init_sta();

    // Mark firmware as valid — rollback protection.
    // If we reach this point, basic init succeeded (NVS, netif, WiFi/Ethernet drivers).
    // If a bad OTA causes a crash before here, the bootloader will revert automatically.
    esp_ota_mark_app_valid_cancel_rollback();

    // Physical recovery / OTA path
    ota_button_init();

    xTaskCreate(status_led_task, "status_led_task", 2048, NULL, 5, NULL);

    // Start 30s one-shot timer — if we survive this long, mark boot healthy
    // (resets the panic counter so normal operation isn't penalized)
    TimerHandle_t health_timer = xTimerCreate(
        "boot_health", pdMS_TO_TICKS(30000), pdFALSE, NULL, boot_health_timer_cb);
    if (health_timer != NULL)
    {
        xTimerStart(health_timer, 0);
    }
}
