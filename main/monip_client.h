#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Destination bitmask for commands
#define MONIP_DEST_A   0x01
#define MONIP_DEST_B   0x02
#define MONIP_DEST_ALL 0x03

// Per-receiver connection state (Monip v2 state machine). A and B are tracked
// independently — each receiver owns its own state, keepalive and reconnection.
typedef enum {
    MONIP_STATE_IDLE = 0,     // no connection requested / not configured
    MONIP_STATE_CONNECTING,   // TCP connect + handshake in progress
    MONIP_STATE_CONNECTED,    // socket up, autonomous keepalive running
    MONIP_STATE_RECONNECTING, // lost connection, internal recovery in progress
    MONIP_STATE_DISCONNECTED, // recovery exhausted (HOST notified in a later phase)
} monip_state_t;

// Command types for the async Monip task
typedef enum {
    MONIP_CMD_CONNECT,       // CMD 0x30
    MONIP_CMD_KEEPALIVE,     // CMD 0x32
    MONIP_CMD_SEND_EVENT,    // CMD 0x01/02/03
    MONIP_CMD_FORWARD,       // CMD 0x35
    MONIP_CMD_DISCONNECT,    // CMD 0x38
} monip_cmd_type_t;

// Command descriptor — submitted to the Monip task queue
typedef struct {
    monip_cmd_type_t type;
    uint8_t dest;           // MONIP_DEST_A, MONIP_DEST_B, or MONIP_DEST_ALL
    uint8_t *payload;       // heap-allocated payload (ownership transferred), NULL if not needed
    uint8_t payload_len;
    uint8_t origin_cmd;     // original CMD byte (used for building the UART response)
} monip_cmd_t;

// Initialize the Monip client: loads config from NVS, creates command queue and tasks
void monip_init(void);

// Submit a command to the Monip task (non-blocking).
// Returns true if enqueued successfully, false if queue full.
// On success, ownership of cmd->payload is transferred to the Monip task.
bool monip_submit_cmd(monip_cmd_t *cmd);

// Query connection status of receivers (for telemetry)
bool monip_receiver_a_connected(void);
bool monip_receiver_b_connected(void);
