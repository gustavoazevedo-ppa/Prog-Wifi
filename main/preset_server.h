#ifndef PRESET_SERVER_H
#define PRESET_SERVER_H

#include <stdbool.h>
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

#endif // PRESET_SERVER_H
