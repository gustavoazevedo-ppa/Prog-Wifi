#ifndef PRESET_SERVER_H
#define PRESET_SERVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_http_server.h"

/**
 * Local HTTP API used by the Stinger (see docs/preset-api-spec.md):
 *   POST /iniciar  - fetch the preset (exemplo.conf) and start the tests
 *   GET  /status   - state of the current run
 *
 * The server listens on port 80 on all interfaces (Wi-Fi and Ethernet).
 * Test actions are only logged for now (UART protocol is not implemented yet).
 * Both endpoints require the X-API-Key header (constant in preset_server.c).
 *
 * Call once after the network stack is initialised (esp_netif_init()).
 */
void preset_server_start(void);

/* True once the HTTP server is up (used to gate the OTA rollback validation). */
bool preset_server_running(void);

/* True while a test run is in progress (admin actions are refused meanwhile). */
bool preset_server_run_active(void);

/* Validate "Authorization: Basic" against the console credentials
 * (CONSOLE_AUTH_USER / CONSOLE_AUTH_PASS in preset_server.c). */
bool preset_server_basic_auth_ok(httpd_req_t *req);

/* ─── Run history and last preset (read by the admin API) ─────────────────── */

#define PRESET_HISTORY_LEN 20  /* requests kept (RAM only, since boot) */
#define PRESET_SOURCE_MAX 448  /* size of the preset source URL buffer */
#define PRESET_BIN_MAX 2048    /* max decoded preset size (bytes) */

typedef enum
{
    PRESET_HRES_RUNNING = 0, /* accepted, reply not decided yet */
    PRESET_HRES_OK,          /* 200: preset found and run started */
    PRESET_HRES_ERROR,       /* accepted but failed (404 / 500) */
    PRESET_HRES_REJECTED,    /* never started (400 / 401 / 409) */
} preset_hist_result_t;

typedef enum
{
    PRESET_LVL_NA = 0,   /* not applicable / unknown */
    PRESET_LVL_CUSTOMER, /* {caminho}/{cliente}/exemplo.conf */
    PRESET_LVL_STATE,    /* {caminho}/{estado}/exemplo.conf */
    PRESET_LVL_DEFAULT,  /* {caminho}/exemplo.conf */
    PRESET_LVL_NONE,     /* every location returned 404 */
} preset_level_t;

typedef struct
{
    uint32_t seq;        /* 1, 2, 3 ... since boot (every request, also rejected ones) */
    uint32_t start_s;    /* uptime (s) when the request arrived */
    preset_hist_result_t result;
    uint16_t code;       /* HTTP status sent, 0 while running */
    preset_level_t level;
    uint16_t bytes;      /* decoded preset size, 0 if none */
    bool has_crc;
    uint32_t crc32;      /* CRC-32 of the decoded preset */
    uint32_t duration_ms;
    char id[24];         /* "" if the request had no usable id */
    char product[24];
    char cliente[16];
    char estado[8];
} preset_history_entry_t;

/* Copies up to max entries, newest first. Returns how many. */
size_t preset_server_history_snapshot(preset_history_entry_t *out, size_t max);

/* Details of the last preset that was downloaded and decoded successfully. */
typedef struct
{
    uint32_t seq;        /* history sequence number of the run that loaded it */
    char id[24];
    char source[PRESET_SOURCE_MAX]; /* URL it came from */
    preset_level_t level;
    uint16_t bytes;
    uint32_t crc32;
    uint32_t loaded_s;   /* uptime (s) when it was loaded */
} preset_info_t;

/* Consistent copy of the last preset (metadata + bytes). Returns false if there is none
 * yet (since boot) or bin_cap is too small. */
bool preset_server_preset_snapshot(preset_info_t *info, uint8_t *bin, size_t bin_cap, size_t *bin_len);

#endif // PRESET_SERVER_H
