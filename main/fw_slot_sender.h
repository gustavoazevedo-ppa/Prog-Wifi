/****************************************************************************
 * Central Firmware Slot Sender
 *
 * Downloads an already-encrypted central-firmware file over HTTPS and relays
 * it to the alarm panel (central MCU) over UART using the slot protocol.
 *
 * The module is mostly "dumb" about the firmware:
 *   - it does NOT encrypt (the blob arrives already encrypted),
 *   - it does NOT interpret `perm` (forwarded verbatim from the header),
 *   - it DOES compute a running CRC32 over the streamed blob as an end-to-end
 *     integrity check: the chained per-slot CRC32 must equal the header crc32.
 *     This verdict gates the terminal result (success vs. failure); the header
 *     crc32 is still forwarded verbatim in metadata regardless.
 *
 * File format (12-byte header + encrypted blob), little-endian size/crc:
 *
 *     perm(4 raw) | firmware_size(4 LE) | crc32(4 LE) | encrypted blob...
 *
 * Transmission (module -> central) uses the directional envelope:
 *     AA AA | LENGTH(2B BE) | type(1B=0x02) | payload(cmd + data + crc8) | BB BB
 *   - metadata payload: 0x0A + perm(4) + firmware_size(4 LE) + crc32(4 LE) + crc8
 *   - slot payload:     0x0A + slot_index(4 LE) + slot_bytes + crc8
 *
 * ACK (central -> module) rides the normal RX path as a PKT_TYPE_CONFIG (0x02)
 * packet with cmd == 0x0A; handle_config_packet routes its status byte here via
 * fw_slot_sender_deliver_ack().
 ****************************************************************************/

#ifndef FW_SLOT_SENDER_H
#define FW_SLOT_SENDER_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Command byte identifying firmware-update traffic (metadata, slot, ACK). */
#define FW_SLOT_CMD 0x0A

/**
 * @brief Status callback for the central-firmware transfer.
 *
 * Invoked from the sender task at terminal states so the caller can report
 * progress (e.g. publish over MQTT) without the sender knowing about topics.
 * `detail` may be NULL.
 *
 * @param status  short status string ("central_done", "central_failed", ...).
 * @param detail  optional human-readable detail, or NULL.
 */
typedef void (*fw_slot_status_cb_t)(const char *status, const char *detail);

/**
 * @brief Register the status callback. Pass NULL to disable reporting.
 *
 * Called once at init (e.g. from ota_mqtt_init). The callback runs on the
 * sender task context.
 */
void fw_slot_sender_set_status_cb(fw_slot_status_cb_t cb);

/**
 * @brief Start the central-firmware update from an MQTT "central" command.
 *
 * Fetches the central manifest (central.json), reads the `url` of the encrypted
 * firmware file (no version check), then downloads and streams it to the central
 * over UART. Runs the whole process on its own task; returns immediately.
 *
 * Safe to call once at a time; a second call while a transfer is in progress is
 * rejected.
 */
void fw_slot_sender_start(void);

/**
 * @brief Deliver a firmware-update ACK status to the waiting sender.
 *
 * Called from the config packet handler when a PKT_TYPE_CONFIG packet with
 * cmd == FW_SLOT_CMD is received. `status` is the byte following the cmd
 * (0 = success). No-op if no transfer is in progress.
 *
 * @param status  ACK status byte from the central.
 */
void fw_slot_sender_deliver_ack(uint8_t status);

/**
 * @brief True while a central-firmware transfer is in progress.
 */
bool fw_slot_sender_busy(void);

#endif // FW_SLOT_SENDER_H
