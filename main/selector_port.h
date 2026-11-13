/*
 * Selector ports: one UART each, a protocol role, framing, reply timing and link supervision.
 *  - SELECTOR_ROLE_SELECTOR: faces the central, the Prog plays the selector (replies at once
 *    from cached keys).
 *  - SELECTOR_ROLE_HOST: faces the real selector, the Prog plays the host (sends its own screens
 *    and polls on a fixed cadence).
 * Ports exchange only messages with the bridge (see selector_bridge.h); they know nothing about
 * what the screens mean. Design: docs/selector-bridge-design.md
 */
#ifndef SELECTOR_PORT_H
#define SELECTOR_PORT_H

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "driver/uart.h"
#include "selector_wire.h"

typedef enum
{
    SELECTOR_ROLE_SELECTOR = 0, // Prog plays the selector (central side)
    SELECTOR_ROLE_HOST          // Prog plays the host (real selector side)
} selector_role_t;

typedef enum
{
    SELECTOR_SIDE_CENTRAL = 0,
    SELECTOR_SIDE_SELECTOR
} selector_side_t;

typedef enum
{
    SELECTOR_EVT_MSG = 0, // a message: dir and msg are valid
    SELECTOR_EVT_LINK,    // link change: side and up are valid
    SELECTOR_EVT_MODE     // central mode change (planned, master mode): up = central is slave
} selector_evt_type_t;

// Port to bridge event
typedef struct
{
    selector_evt_type_t type;
    selector_side_t side;
    selector_dir_t dir;
    bool up;
    selector_msg_t msg;
} selector_evt_t;

typedef struct
{
    uint32_t frames_ok;
    uint32_t drops[SELECTOR_DROP_REASON_COUNT];
    uint32_t queue_full;
    uint32_t reply_timeouts; // host role only
    uint32_t link_changes;
    bool link_up;
} selector_port_stats_t;

typedef struct
{
    selector_role_t role;
    uart_port_t uart;
    int tx_pin;
    int rx_pin;
    uint32_t baud;
    uint32_t partial_timeout_ms;
    uint32_t link_timeout_ms;
    uint32_t reply_delay_ms;   // selector role (faces the central): delay between the end of the
                               // central's frame and the start of the reply, 0 = at once
    uint32_t poll_ms;          // host role: key poll interval while the selector answers
    uint32_t down_poll_ms;     // host role: poll interval while the selector does not answer
    uint32_t refresh_ms;       // host role: resend the screen after this long without a change
    uint32_t reply_timeout_ms; // host role: time allowed, after the end of the host frame,
                               // for the reply to complete
    QueueHandle_t to_bridge;   // selector_evt_t, written by the port
    QueueHandle_t from_bridge; // selector_msg_t, read by the port
} selector_port_cfg_t;

typedef struct selector_port selector_port_t;

// Installs the UART and starts the port task. Returns NULL on failure.
selector_port_t *selector_port_start(const selector_port_cfg_t *cfg);

void selector_port_get_stats(const selector_port_t *port, selector_port_stats_t *out);

// Logs valid frames (hex, INFO level) on all ports while enabled. A frame identical to the
// previous one in the same direction on the same port is skipped, and the next different frame
// says how many repeats were skipped (TAP_CHANGES_ONLY in selector_port.c).
void selector_port_set_tap(bool on);

#endif // SELECTOR_PORT_H
