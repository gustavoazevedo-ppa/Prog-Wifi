#include "device_restart.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs.h"
#include <string.h>
#include <stdio.h>

#define TAG         "DEV_RST"
#define NVS_NS      "device"
#define NVS_KEY     "rst_reason"

static const char *hw_reason_str(esp_reset_reason_t r)
{
    switch (r) {
        case ESP_RST_POWERON:   return "power_on";
        case ESP_RST_EXT:       return "external";
        case ESP_RST_SW:        return "software";
        case ESP_RST_PANIC:     return "panic";
        case ESP_RST_INT_WDT:   return "int_wdt";
        case ESP_RST_TASK_WDT:  return "task_wdt";
        case ESP_RST_WDT:       return "wdt";
        case ESP_RST_DEEPSLEEP: return "deepsleep";
        case ESP_RST_BROWNOUT:  return "brownout";
        case ESP_RST_SDIO:      return "sdio";
        default:                return "unknown";
    }
}

void device_set_restart_reason(const char *reason)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, NVS_KEY, reason ? reason : "unknown");
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGW(TAG, "Restart reason recorded: %s", reason ? reason : "unknown");
}

void device_restart(const char *reason)
{
    device_set_restart_reason(reason);
    ESP_LOGW(TAG, "Restarting (reason: %s)", reason ? reason : "unknown");
    esp_restart();
}

void device_get_last_restart_info(char *hw_buf, size_t hw_size,
                                  char *sw_buf, size_t sw_size)
{
    esp_reset_reason_t hw = esp_reset_reason();
    const char *hw_str = hw_reason_str(hw);
    strncpy(hw_buf, hw_str, hw_size - 1);
    hw_buf[hw_size - 1] = '\0';

    sw_buf[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        size_t len = sw_size;
        nvs_get_str(h, NVS_KEY, sw_buf, &len);
        /* Clear after reading — one-shot */
        nvs_erase_key(h, NVS_KEY);
        nvs_commit(h);
        nvs_close(h);
    }
}
