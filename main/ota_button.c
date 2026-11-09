/****************************************************************************
 * OTA Update Button Handler
 *
 * Button on GPIO34 (pull-up, switch to GND).
 * Hold >= 10s to trigger:
 *   1. Both LEDs flash slowly while checking for OTA update
 *   2. Flash speed increases as update progresses
 *   3. Flash format is performed (even if OTA fails/unavailable)
 *   4. Fastest flash to indicate completion, then reset
 ****************************************************************************/

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_app_desc.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_encrypted_img.h"
#include "esp_ota_ops.h"
#include "esp_tls.h"
#include "esp_crt_bundle.h"
#include "nvs_flash.h"
#include "cJSON.h"
#include "ota_button.h"
#include "device_info.h"

#define TAG "OTA_BTN"

#define OTA_BUTTON_PIN      GPIO_NUM_34
#define WIFI_LED_PIN        GPIO_NUM_33
#define MQTT_LED_PIN        GPIO_NUM_32

/* Flag: when true, OTA process owns the LEDs — other tasks must not touch them */
static volatile bool s_leds_busy = false;

/* Button is active-low (pressed = 0) */
#define BUTTON_PRESSED()    (gpio_get_level(OTA_BUTTON_PIN) == 0)

/* Hold time required to trigger OTA process (ms) */
#define HOLD_THRESHOLD_MS   10000

/* LED blink periods (ms) for each stage */
#define BLINK_SLOW_MS       500
#define BLINK_MEDIUM_MS     250
#define BLINK_FAST_MS       100
#define BLINK_FASTEST_MS    50

/* How long to blink at fastest speed before reset (ms) */
#define DONE_BLINK_DURATION_MS  3000

/* Polling interval for button state (ms) */
#define POLL_INTERVAL_MS    50

/* Max size for manifest JSON download (bytes) */
#define MANIFEST_MAX_SIZE   1024

/* --------------------------------------------------------------------------
 * LED blink task — runs independently, blinks at s_blink_interval_ms rate
 * -------------------------------------------------------------------------- */

static volatile uint32_t s_blink_interval_ms = 0; /* 0 = off */
static TaskHandle_t s_blink_task_handle = NULL;

static void leds_set(bool on)
{
    gpio_set_level(MQTT_LED_PIN, on ? 0 : 1);
}

static void led_blink_task(void *arg)
{
    bool state = false;

    while (1) {
        uint32_t interval = s_blink_interval_ms;
        if (interval == 0) {
            leds_set(false);
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        state = !state;
        leds_set(state);
        vTaskDelay(pdMS_TO_TICKS(interval / 2));
    }
}

static void led_blink_start(uint32_t interval_ms)
{
    s_blink_interval_ms = interval_ms;
    if (s_blink_task_handle == NULL) {
        xTaskCreate(led_blink_task, "led_blink", 1024, NULL, 4, &s_blink_task_handle);
    }
}

static void led_blink_stop(void)
{
    s_blink_interval_ms = 0;
    /* Give the task time to see the zero and turn off the LED */
    vTaskDelay(pdMS_TO_TICKS(100));
}

/* --------------------------------------------------------------------------
 * Memory management — free RAM before OTA
 * -------------------------------------------------------------------------- */

/**
 * @brief Log heap state before OTA. This is the hook point for releasing any
 * application services (tasks, clients) to reclaim RAM for TLS/OTA buffers.
 * Since we reset after OTA, there's no need to restart them.
 */
static void free_memory_for_ota(void)
{
    ESP_LOGI(TAG, "Free heap before OTA: %lu bytes (largest block: %lu)",
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

/* --------------------------------------------------------------------------
 * Semver comparison helper
 * -------------------------------------------------------------------------- */

/**
 * @brief Compare two semver strings (major.minor.patch).
 * @return  >0 if a > b, 0 if equal, <0 if a < b.
 */
static int semver_compare(const char *a, const char *b)
{
    int a_major = 0, a_minor = 0, a_patch = 0;
    int b_major = 0, b_minor = 0, b_patch = 0;

    sscanf(a, "%d.%d.%d", &a_major, &a_minor, &a_patch);
    sscanf(b, "%d.%d.%d", &b_major, &b_minor, &b_patch);

    if (a_major != b_major) return a_major - b_major;
    if (a_minor != b_minor) return a_minor - b_minor;
    return a_patch - b_patch;
}

/* --------------------------------------------------------------------------
 * Manifest download
 * -------------------------------------------------------------------------- */

/**
 * @brief Download the JSON manifest from S3.
 *
 * @param[out] out_buf     Buffer to write JSON into (null-terminated).
 * @param[in]  buf_size    Size of out_buf.
 * @return ESP_OK on success, error otherwise.
 */
static esp_err_t download_manifest(const char *manifest_url, char *out_buf, size_t buf_size)
{
    esp_http_client_config_t config = {
        .url = manifest_url,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 1024,
        .buffer_size_tx = 512,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    int content_length = esp_http_client_fetch_headers(client);
    if (content_length < 0 || (size_t)content_length >= buf_size) {
        ESP_LOGE(TAG, "Manifest too large or fetch failed (len=%d)", content_length);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    int read_len = esp_http_client_read(client, out_buf, buf_size - 1);
    if (read_len <= 0) {
        ESP_LOGE(TAG, "Failed to read manifest body");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }
    out_buf[read_len] = '\0';

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ESP_OK;
}

/* --------------------------------------------------------------------------
 * Pre-encrypted OTA decrypt layer
 * -------------------------------------------------------------------------- */

/* On-device RSA-3072 private key, embedded via EMBED_TXTFILES in CMakeLists.txt.
 * Used to unwrap the AES-GCM session key of a pre-encrypted OTA image. */
extern const char ota_decrypt_priv_pem_start[] asm("_binary_ota_decrypt_priv_pem_start");
extern const char ota_decrypt_priv_pem_end[]   asm("_binary_ota_decrypt_priv_pem_end");

/**
 * @brief esp_https_ota decrypt callback — feeds each downloaded chunk through
 *        esp_encrypted_img and returns the decrypted bytes.
 *
 * The firmware version / HW gating already happened against the manifest before
 * the download started, so this callback only performs decryption (no image
 * header re-validation). ESP_ERR_NOT_FINISHED means "more data needed" and is
 * treated as success-in-progress.
 */
static esp_err_t ota_decrypt_cb(decrypt_cb_arg_t *args, void *user_ctx)
{
    if (args == NULL || user_ctx == NULL) {
        ESP_LOGE(TAG, "decrypt_cb: invalid argument");
        return ESP_ERR_INVALID_ARG;
    }

    pre_enc_decrypt_arg_t pargs = {
        .data_in = args->data_in,
        .data_in_len = args->data_in_len,
    };

    esp_err_t err = esp_encrypted_img_decrypt_data((esp_decrypt_handle_t)user_ctx, &pargs);
    if (err != ESP_OK && err != ESP_ERR_NOT_FINISHED) {
        ESP_LOGE(TAG, "decrypt_cb: decrypt failed: %s", esp_err_to_name(err));
        free(pargs.data_out);
        return err;
    }

    if (pargs.data_out_len > 0) {
        args->data_out = pargs.data_out;
        args->data_out_len = pargs.data_out_len;
    } else {
        args->data_out_len = 0;
    }

    return ESP_OK;
}

/* --------------------------------------------------------------------------
 * OTA update implementation
 * -------------------------------------------------------------------------- */

/**
 * @brief Check for and apply an OTA update.
 *
 * 1. Downloads firmware.json manifest from S3
 * 2. Compares remote version against running version
 * 3. If newer, downloads and flashes the binary via esp_https_ota
 *
 * @param[out] progress_pct  Pointer updated with progress 0-100 during download.
 * @param[in]  apply         false = only check the manifest (nothing is downloaded);
 *                           true  = also download and flash the new image.
 * @param[in]  leds          true = blink the LED while downloading (button flow);
 *                           false = leave the LEDs alone (HTTP admin flow).
 * @param[out] remote_ver    Optional: receives the manifest version string.
 * @param[in]  remote_ver_len Size of remote_ver.
 * @return  ESP_OK if an update was applied (apply) or is available (check only),
 *          ESP_ERR_NOT_FOUND if no update available (same/older version),
 *          other esp_err_t on failure.
 */
static esp_err_t ota_check_and_apply(const char *manifest_url, uint8_t *progress_pct, bool apply, bool leds,
                                     char *remote_ver, size_t remote_ver_len)
{
    if (progress_pct) *progress_pct = 0;
    if (remote_ver && remote_ver_len > 0) remote_ver[0] = '\0';

    /* Step 1: Download manifest */
    char manifest_buf[MANIFEST_MAX_SIZE];
    esp_err_t err = download_manifest(manifest_url, manifest_buf, sizeof(manifest_buf));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to download manifest");
        return err;
    }

    ESP_LOGI(TAG, "Manifest: %s", manifest_buf);

    /* Step 2: Parse JSON */
    cJSON *root = cJSON_Parse(manifest_buf);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse manifest JSON");
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON *version_json = cJSON_GetObjectItem(root, "version");
    cJSON *url_json = cJSON_GetObjectItem(root, "url");
    cJSON *hw_json = cJSON_GetObjectItem(root, "hw");

    if (!cJSON_IsString(version_json) || !cJSON_IsString(url_json)) {
        ESP_LOGE(TAG, "Manifest missing 'version' or 'url' fields");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    /* Safety check: verify manifest is for our hardware */
    if (cJSON_IsNumber(hw_json)) {
        int manifest_hw = hw_json->valueint;
        if (manifest_hw != HW_VERSION) {
            ESP_LOGE(TAG, "HW mismatch! Manifest hw=%d, device hw=%d", manifest_hw, HW_VERSION);
            cJSON_Delete(root);
            return ESP_ERR_INVALID_VERSION;
        }
    } else {
        ESP_LOGW(TAG, "Manifest missing 'hw' field, skipping HW check");
    }

    const char *remote_version = version_json->valuestring;
    const char *firmware_url = url_json->valuestring;

    if (remote_ver && remote_ver_len > 0) {
        strlcpy(remote_ver, remote_version, remote_ver_len);
    }

    /* Step 3: Compare versions */
    const esp_app_desc_t *app_desc = esp_app_get_description();
    ESP_LOGI(TAG, "Running: %s, Remote: %s", app_desc->version, remote_version);

    if (semver_compare(remote_version, app_desc->version) <= 0) {
        ESP_LOGI(TAG, "No newer firmware available");
        cJSON_Delete(root);
        return ESP_ERR_NOT_FOUND;
    }

    if (!apply) {
        ESP_LOGI(TAG, "Newer firmware available (%s)", remote_version);
        cJSON_Delete(root);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Newer firmware found (%s), downloading from: %s", remote_version, firmware_url);
    if (progress_pct) *progress_pct = 10;

    /* Step 4: Start the pre-encryption decrypt layer. The image on S3 is
     * RSA-3072 wrapped; esp_encrypted_img unwraps it on-device using the
     * embedded private key. */
    esp_decrypt_cfg_t decrypt_cfg = {
        .rsa_priv_key = ota_decrypt_priv_pem_start,
        .rsa_priv_key_len = ota_decrypt_priv_pem_end - ota_decrypt_priv_pem_start,
    };
    esp_decrypt_handle_t decrypt_handle = esp_encrypted_img_decrypt_start(&decrypt_cfg);
    if (decrypt_handle == NULL) {
        ESP_LOGE(TAG, "Failed to start OTA decrypt layer");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    /* Step 5: Perform OTA */
    esp_http_client_config_t ota_http_config = {
        .url = firmware_url,
        .timeout_ms = 60000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
    };

    esp_https_ota_config_t ota_config = {
        .http_config = &ota_http_config,
        .decrypt_cb = ota_decrypt_cb,
        .decrypt_user_ctx = (void *)decrypt_handle,
        .enc_img_header_size = esp_encrypted_img_get_header_size(),
    };

    esp_https_ota_handle_t ota_handle = NULL;
    err = esp_https_ota_begin(&ota_config, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA begin failed: %s", esp_err_to_name(err));
        esp_encrypted_img_decrypt_abort(decrypt_handle);
        cJSON_Delete(root);
        return err;
    }

    /* Download in chunks, updating progress */
    int total_size = esp_https_ota_get_image_size(ota_handle);
    int downloaded = 0;

    /* Start LED blink — slow initially, speeds up with progress */
    if (leds) led_blink_start(BLINK_SLOW_MS);

    while (1) {
        err = esp_https_ota_perform(ota_handle);
        if (err == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            downloaded = esp_https_ota_get_image_len_read(ota_handle);
            if (total_size > 0 && progress_pct) {
                /* Scale progress: 10-95% for download phase */
                *progress_pct = 10 + (uint8_t)((downloaded * 85L) / total_size);
            }
            /* Accelerate blink as download progresses */
            if (leds && total_size > 0) {
                uint32_t interval = BLINK_SLOW_MS - (uint32_t)(((BLINK_SLOW_MS - BLINK_FAST_MS) * downloaded) / total_size);
                s_blink_interval_ms = interval;
            }
            continue;
        }
        break;
    }
    if (leds) led_blink_stop();

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA perform failed: %s", esp_err_to_name(err));
        esp_https_ota_abort(ota_handle);
        esp_encrypted_img_decrypt_abort(decrypt_handle);
        cJSON_Delete(root);
        return err;
    }

    if (progress_pct) *progress_pct = 95;

    /* Finalize the decrypt layer before committing the image. A failure here
     * means the complete image wasn't decrypted, so treat it as an OTA abort. */
    err = esp_encrypted_img_decrypt_end(decrypt_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA decrypt end failed: %s", esp_err_to_name(err));
        esp_https_ota_abort(ota_handle);
        cJSON_Delete(root);
        return err;
    }

    err = esp_https_ota_finish(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA finish failed: %s", esp_err_to_name(err));
        cJSON_Delete(root);
        return err;
    }

    if (progress_pct) *progress_pct = 100;
    ESP_LOGI(TAG, "OTA update successful, new version: %s", remote_version);

    cJSON_Delete(root);
    return ESP_OK;
}

/* --------------------------------------------------------------------------
 * NVS format
 * -------------------------------------------------------------------------- */

/**
 * @brief Erase NVS partition (factory reset).
 *
 * This always runs regardless of OTA result to restore factory defaults.
 * Erases the entire NVS partition so all stored credentials, settings,
 * and schema data are wiped. After the subsequent restart, nvs_flash_init()
 * in app_main will re-create a clean NVS.
 */
static esp_err_t flash_format(void)
{
    ESP_LOGW(TAG, "Erasing NVS partition...");
    esp_err_t err = nvs_flash_erase();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS erase failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "NVS erased successfully");
    }
    return err;
}

/* --------------------------------------------------------------------------
 * Public helpers used by the HTTP admin API (admin_api.c).
 * Same logic as the button flow, but no LED handling and no automatic
 * NVS erase / restart: the caller decides.
 * -------------------------------------------------------------------------- */

esp_err_t ota_check_update(const char *manifest_url, char *remote_ver, size_t remote_ver_len)
{
    return ota_check_and_apply(manifest_url, NULL, false, false, remote_ver, remote_ver_len);
}

esp_err_t ota_apply_update(const char *manifest_url, volatile uint8_t *progress_pct,
                           char *remote_ver, size_t remote_ver_len)
{
    return ota_check_and_apply(manifest_url, (uint8_t *)progress_pct, true, false, remote_ver, remote_ver_len);
}

esp_err_t ota_format_nvs(void)
{
    return flash_format();
}

/* --------------------------------------------------------------------------
 * OTA process (runs after button held for 10s)
 * -------------------------------------------------------------------------- */

static void ota_process(void)
{
    uint8_t progress = 0;

    /* Take ownership of the LEDs so status_led_task stops overriding them */
    s_leds_busy = true;

    /* Release BLE stack to free ~60KB for TLS buffers */
    free_memory_for_ota();

    ESP_LOGI(TAG, "OTA process started");

    /* Phase 1: Checking for update — slow blink while downloading */
    ESP_LOGI(TAG, "Checking for OTA update...");
    led_blink_start(BLINK_SLOW_MS);

    esp_err_t ota_result = ota_check_and_apply(OTA_MANIFEST_URL, &progress, true, true, NULL, 0);

    if (ota_result == ESP_OK) {
        ESP_LOGI(TAG, "OTA update applied successfully");
    } else if (ota_result == ESP_ERR_NOT_FOUND) {
        ESP_LOGI(TAG, "No OTA update available");
    } else {
        ESP_LOGE(TAG, "OTA update failed: %s", esp_err_to_name(ota_result));
    }

    /* Phase 2: Flash format — always performed */
    ESP_LOGI(TAG, "Performing NVS erase...");
    s_blink_interval_ms = BLINK_FAST_MS;
    flash_format();

    /* Phase 3: Done — fastest blink to indicate completion */
    ESP_LOGI(TAG, "Process complete, blinking fast then resetting...");
    s_blink_interval_ms = BLINK_FASTEST_MS;
    vTaskDelay(pdMS_TO_TICKS(DONE_BLINK_DURATION_MS));

    /* Reset the device. No restart reason is written here: flash_format()
     * wiped NVS, so any reason would be lost. Instead, nvs_schema_check()
     * on the next boot detects the missing schema and records "nvs_format". */
    led_blink_stop();
    ESP_LOGI(TAG, "Restarting...");
    esp_restart();
}

/* --------------------------------------------------------------------------
 * Button monitoring task
 * -------------------------------------------------------------------------- */

static void ota_button_task(void *arg)
{
    uint32_t press_duration_ms = 0;
    bool was_pressed = false;

    while (1) {
        if (BUTTON_PRESSED()) {
            press_duration_ms += POLL_INTERVAL_MS;

            if (!was_pressed) {
                was_pressed = true;
                press_duration_ms = POLL_INTERVAL_MS;
            }

            if (press_duration_ms >= HOLD_THRESHOLD_MS) {
                s_leds_busy = true; /* Claim LEDs so status task backs off */
                ESP_LOGI(TAG, "Button held for 10s — starting OTA process");
                /* Button released after threshold — trigger OTA process */
                ota_process();
                /* ota_process calls esp_restart, so we never reach here */
            }
        } else {
            /* Reset state */
            was_pressed = false;
            press_duration_ms = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

void ota_button_init(void)
{
    /* Configure GPIO34 as input — note: GPIO34-39 on ESP32 are input-only
     * and do NOT have internal pull-ups. An external pull-up resistor is
     * required on the PCB. We just configure as input here. */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << OTA_BUTTON_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,   /* GPIO34 has no internal pull-up */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    ESP_LOGI(TAG, "OTA button initialized on GPIO%d", OTA_BUTTON_PIN);

    xTaskCreate(ota_button_task, "ota_button_task", 8192, NULL, 5, NULL);
}

bool ota_button_leds_busy(void)
{
    return s_leds_busy;
}
