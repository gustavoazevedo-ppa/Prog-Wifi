/*
 * Selector bridge: sits between the central (UART2) and the real selector (UART1).
 * Two ports speak the protocol on each side; a translator in the middle sees only messages
 * (screens, key states, beeps, backlight) and decides what to forward, change or generate.
 * Design: docs/selector-bridge-design.md
 */
#ifndef SELECTOR_BRIDGE_H
#define SELECTOR_BRIDGE_H

#include <stdint.h>
#include <stdbool.h>
#include "selector_wire.h"
#include "selector_port.h"

// A translator is a stateful, time-driven object. All its callbacks run in the bridge task, so
// it needs no locks and may call selector_bridge_emit() from them. Callbacks may be NULL.
typedef struct
{
    void *ctx;
    // A downstream message from the central (SCREEN, POLL, BEEP, BACKLIGHT), or an upstream
    // key state from the real selector (KEYS).
    void (*on_msg)(void *ctx, selector_dir_t dir, const selector_msg_t *msg);
    void (*on_link)(void *ctx, selector_side_t side, bool up);
    // Central mode change (planned, master mode): true when the central is slave.
    void (*on_mode)(void *ctx, bool central_slave);
    // Every SELECTOR_BRIDGE_TICK_MS
    void (*on_tick)(void *ctx, uint32_t now_ms);
} selector_translator_t;

#define SELECTOR_BRIDGE_TICK_MS 10

typedef struct
{
    selector_port_stats_t panel;    // faces the central
    selector_port_stats_t selector; // faces the real selector
    uint32_t bridge_queue_full;
} selector_bridge_stats_t;

// Sends a message out of the bridge. Downstream goes to the real selector (SCREEN, BEEP,
// BACKLIGHT), upstream goes to the central (KEYS). Other types are ignored.
// Only call it from the translator callbacks.
void selector_bridge_emit(selector_dir_t dir, const selector_msg_t *msg);

// Milliseconds since boot (wraps at 2^32, compare with subtraction).
uint32_t selector_bridge_now_ms(void);

// Pass-through translator: forwards everything unchanged in its own direction.
const selector_translator_t *selector_tr_identity(void);

// Creates the queues, starts both ports and the bridge task. A NULL translator means identity.
// UART2 faces the central (TX GPIO17, RX GPIO16), UART1 faces the selector (TX GPIO25,
// RX GPIO26), both 9600 8N1. Returns false on failure (the rest of the firmware keeps running).
bool selector_bridge_start(const selector_translator_t *translator);

void selector_bridge_get_stats(selector_bridge_stats_t *out);

// Logs every valid frame on both ports (hex, INFO level). Off by default.
void selector_bridge_set_tap(bool on);

#endif // SELECTOR_BRIDGE_H
