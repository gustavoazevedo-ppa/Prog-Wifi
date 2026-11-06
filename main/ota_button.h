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
 * @brief Check if the OTA process currently owns the LEDs.
 *
 * Other tasks (e.g. status_led_task) should skip writing to the LEDs
 * when this returns true.
 */
bool ota_button_leds_busy(void);

#endif // OTA_BUTTON_H
