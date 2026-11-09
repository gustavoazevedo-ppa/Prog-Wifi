/****************************************************************************
 * OTA Update Button Handler
 *
 * GPIO34 button with pull-up, switch to GND.
 * Press >= 10s triggers OTA check + flash format + reset.
 * LEDs flash during the process to indicate progress.
 ****************************************************************************/

#ifndef OTA_BUTTON_H
#define OTA_BUTTON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "device_info.h"

/* Helper to stringify a macro value */
#define STRINGIFY_(x) #x
#define STRINGIFY(x)  STRINGIFY_(x)

/* OTA manifest URLs — per hardware version */
#define OTA_MANIFEST_URL       "https://download.citrox.com.br/fw/hw" STRINGIFY(HW_VERSION) "/firmware.json"
#define OTA_MANIFEST_URL_BETA  "https://download.citrox.com.br/fw/hw" STRINGIFY(HW_VERSION) "/beta.json"

/**
 * @brief Initialize the OTA button handler.
 *
 * Configures GPIO34 as input with pull-up and starts the monitoring task.
 * Requires the LED GPIOs (WIFI_LED_PIN, MQTT_LED_PIN) to already be
 * configured as outputs before calling this function.
 */
void ota_button_init(void);

/**
 * @brief Check the manifest only (nothing is downloaded or flashed).
 *
 * Blocking, needs a TLS-capable task stack (>= 8 KB). Does not touch the LEDs.
 *
 * @param manifest_url    OTA_MANIFEST_URL or OTA_MANIFEST_URL_BETA.
 * @param remote_ver      Optional: receives the manifest version string.
 * @param remote_ver_len  Size of remote_ver.
 * @return ESP_OK = newer firmware available, ESP_ERR_NOT_FOUND = up to date,
 *         other esp_err_t = failure.
 */
esp_err_t ota_check_update(const char *manifest_url, char *remote_ver, size_t remote_ver_len);

/**
 * @brief Check the manifest and, if newer, download and flash the image.
 *
 * Blocking (tens of seconds). Does NOT erase NVS and does NOT restart: after
 * ESP_OK the new image is selected as the boot partition and takes effect at
 * the next reboot. Does not touch the LEDs.
 *
 * @param progress_pct  Optional: updated with 0-100 while running.
 * @return ESP_OK = applied, ESP_ERR_NOT_FOUND = nothing newer, other = failure.
 */
esp_err_t ota_apply_update(const char *manifest_url, volatile uint8_t *progress_pct,
                           char *remote_ver, size_t remote_ver_len);

/** @brief Erase the NVS partition (factory reset of stored settings). */
esp_err_t ota_format_nvs(void);

/**
 * @brief Check if the OTA process currently owns the LEDs.
 *
 * Other tasks (e.g. status_led_task) should skip writing to the LEDs
 * when this returns true.
 */
bool ota_button_leds_busy(void);

#endif // OTA_BUTTON_H
