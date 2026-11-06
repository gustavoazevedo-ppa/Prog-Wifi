#ifndef PRESET_SERVER_H
#define PRESET_SERVER_H

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

#endif // PRESET_SERVER_H
