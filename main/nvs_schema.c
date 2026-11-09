#include "nvs_schema.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_system.h"

#define TAG "NVS_SCHEMA"
#define NVS_SCHEMA_NAMESPACE "schema"
#define NVS_SCHEMA_KEY "version"
#define NVS_PANIC_COUNT_KEY "panic_cnt"

#define NVS_BOOT_COUNT_KEY "boot_cnt"
#define NVS_PANIC_TOTAL_KEY "panic_tot"

static uint32_t s_boot_count = 0;
static uint32_t s_panic_total = 0;
static bool s_counters_valid = false;

// Only panic-type resets count as crashes / potential boot loop indicators
static bool reset_is_crash(esp_reset_reason_t reason)
{
    return reason == ESP_RST_PANIC ||
           reason == ESP_RST_INT_WDT ||
           reason == ESP_RST_TASK_WDT ||
           reason == ESP_RST_WDT;
}

void nvs_schema_check(void)
{
    nvs_handle_t h;
    uint32_t stored_version = 0;

    esp_err_t err = nvs_open(NVS_SCHEMA_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to open schema namespace: %s", esp_err_to_name(err));
        // If we can't even open NVS, force a full erase
        ESP_LOGW(TAG, "Erasing NVS due to open failure");
        nvs_flash_erase();
        nvs_flash_init();
        // Try again after erase
        err = nvs_open(NVS_SCHEMA_NAMESPACE, NVS_READWRITE, &h);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "NVS still unusable after erase, giving up");
            return;
        }
    }

    err = nvs_get_u32(h, NVS_SCHEMA_KEY, &stored_version);

    if (err == ESP_OK && stored_version == NVS_SCHEMA_VERSION)
    {
        ESP_LOGI(TAG, "NVS schema version %lu OK", (unsigned long)stored_version);
        nvs_close(h);
        return;
    }

    // Version mismatch or key missing — erase everything and start fresh
    if (err == ESP_OK)
    {
        ESP_LOGW(TAG, "NVS schema version mismatch (stored=%lu, expected=%d). Erasing NVS.",
                 (unsigned long)stored_version, NVS_SCHEMA_VERSION);
    }
    else
    {
        ESP_LOGW(TAG, "No NVS schema version found. Erasing NVS for clean state.");
    }

    nvs_close(h);
    nvs_flash_erase();
    nvs_flash_init();

    // Write the current version
    err = nvs_open(NVS_SCHEMA_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK)
    {
        nvs_set_u32(h, NVS_SCHEMA_KEY, NVS_SCHEMA_VERSION);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "NVS erased and schema version %d written", NVS_SCHEMA_VERSION);
    }
    else
    {
        ESP_LOGE(TAG, "Failed to write schema version after erase");
    }
}

bool nvs_boot_loop_check(void)
{
    esp_reset_reason_t reason = esp_reset_reason();

    // Only count panic-type resets as potential boot loop indicators
    bool is_crash = reset_is_crash(reason);

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_SCHEMA_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "boot_loop_check: can't open NVS");
        return false;
    }

    uint8_t count = 0;
    nvs_get_u8(h, NVS_PANIC_COUNT_KEY, &count); // OK if not found (defaults to 0)

    if (!is_crash)
    {
        // Normal boot (power-on, software reset, etc.) — reset counter
        if (count != 0)
        {
            nvs_set_u8(h, NVS_PANIC_COUNT_KEY, 0);
            nvs_commit(h);
            ESP_LOGI(TAG, "Normal boot, panic counter reset (was %d)", count);
        }
        nvs_close(h);
        return false;
    }

    // It's a crash reboot — increment counter
    count++;
    ESP_LOGW(TAG, "Panic reboot detected (count=%d/%d, reason=%d)",
             count, NVS_MAX_PANIC_BOOTS, (int)reason);

    if (count >= NVS_MAX_PANIC_BOOTS)
    {
        ESP_LOGE(TAG, "Boot loop detected! Erasing NVS to recover...");
        nvs_close(h);
        nvs_flash_erase();
        nvs_flash_init();

        // Re-write schema version so nvs_schema_check doesn't erase again
        err = nvs_open(NVS_SCHEMA_NAMESPACE, NVS_READWRITE, &h);
        if (err == ESP_OK)
        {
            nvs_set_u32(h, NVS_SCHEMA_KEY, NVS_SCHEMA_VERSION);
            nvs_set_u8(h, NVS_PANIC_COUNT_KEY, 0);
            nvs_commit(h);
            nvs_close(h);
        }
        // Note: no "nvs_format" reason is recorded here. This erase was
        // triggered by a crash (panic/watchdog), so the hardware reset reason
        // (panic, task_wdt, ...) is the meaningful signal for telemetry;
        // overwriting it with a vaguer software reason would lose information.
        ESP_LOGW(TAG, "NVS erased due to boot loop. Device will start fresh.");
        return true;
    }

    // Below threshold — just persist the incremented counter
    nvs_set_u8(h, NVS_PANIC_COUNT_KEY, count);
    nvs_commit(h);
    nvs_close(h);
    return false;
}

void nvs_boot_counters_update(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_SCHEMA_NAMESPACE, NVS_READWRITE, &h) != ESP_OK)
    {
        ESP_LOGW(TAG, "boot counters: can't open NVS");
        return;
    }

    uint32_t boot = 0, panic = 0;
    nvs_get_u32(h, NVS_BOOT_COUNT_KEY, &boot);   // OK if not found (starts at 0)
    nvs_get_u32(h, NVS_PANIC_TOTAL_KEY, &panic);

    boot++;
    if (reset_is_crash(esp_reset_reason()))
    {
        panic++;
    }

    nvs_set_u32(h, NVS_BOOT_COUNT_KEY, boot);
    nvs_set_u32(h, NVS_PANIC_TOTAL_KEY, panic);
    if (nvs_commit(h) == ESP_OK)
    {
        s_boot_count = boot;
        s_panic_total = panic;
        s_counters_valid = true;
    }
    nvs_close(h);
}

bool nvs_boot_counters_get(uint32_t *boot_count, uint32_t *panic_total)
{
    if (!s_counters_valid)
    {
        return false;
    }
    if (boot_count != NULL)
    {
        *boot_count = s_boot_count;
    }
    if (panic_total != NULL)
    {
        *panic_total = s_panic_total;
    }
    return true;
}

void nvs_boot_mark_healthy(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_SCHEMA_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK)
        return;

    uint8_t count = 0;
    nvs_get_u8(h, NVS_PANIC_COUNT_KEY, &count);
    if (count != 0)
    {
        nvs_set_u8(h, NVS_PANIC_COUNT_KEY, 0);
        nvs_commit(h);
        ESP_LOGI(TAG, "Boot marked healthy, panic counter cleared");
    }
    nvs_close(h);
}
