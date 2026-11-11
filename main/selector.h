/*
 * Selector (XMC1100 16x2 LCD board) host driver, basic version.
 * Shows Wi-Fi / Ethernet IP and MAC on the LCD, rotating pages.
 * Protocol reference: docs/selector-protocol.md
 */
#ifndef SELECTOR_H
#define SELECTOR_H

#include <stdint.h>
#include <stddef.h>

// Builds a full 38-byte 'S' (screen) frame into out (needs 38 bytes).
// Lines are truncated/space-padded to 16 chars. Returns the frame length.
size_t selector_build_screen(uint8_t *out, const char *line1, const char *line2);

// Checksum over frame[0 .. len-3] (XOR, complemented). len includes checksum and stop.
uint8_t selector_checksum(const uint8_t *frame, size_t len);

// Starts the selector task on UART2 (TX GPIO17, RX GPIO16, 9600 8N1).
void selector_start(void);

#endif // SELECTOR_H
