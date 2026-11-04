#pragma once

#include <stddef.h>

/**
 * @brief Set the application-level restart reason and immediately reboot.
 *
 * The reason is persisted to NVS before calling esp_restart(), so after
 * reboot the backend can know *why* the device restarted.
 *
 * @param reason  Short reason string (max 23 chars), e.g.:
 *                "ota_success", "ota_no_update", "ota_failed", "remote_cmd"
 */
void device_restart(const char *reason);

/**
 * @brief Persist the application-level restart reason without rebooting.
 *
 * Writes the reason to the same NVS key that device_get_last_restart_info()
 * reads (and clears) one-shot. Use this to record a reason from a context
 * that has just (re)initialized NVS and does not itself reboot, e.g. after
 * an NVS format on boot. Overwrites any previously stored reason.
 *
 * @param reason  Short reason string (max 23 chars), e.g. "nvs_format".
 */
void device_set_restart_reason(const char *reason);

/**
 * @brief Get the hardware and software restart reasons separately.
 *
 * Reads the app-level reason from NVS (clears after reading).
 * Returns the hardware reason string and the software reason string.
 *
 * @param hw_buf     Buffer for hardware reason (recommend 12 bytes).
 * @param hw_size    Size of hw_buf.
 * @param sw_buf     Buffer for software/app reason (recommend 24 bytes).
 * @param sw_size    Size of sw_buf.
 */
void device_get_last_restart_info(char *hw_buf, size_t hw_size,
                                  char *sw_buf, size_t sw_size);
