#ifndef LOG_SERVER_H
#define LOG_SERVER_H

#include <stdbool.h>

/**
 * Read-only remote log console (telnet style raw TCP).
 *
 * The TCP port is CLOSED by default so a network scanner sees nothing. It is
 * opened on demand with log_server_request_open() (exposed over HTTP as
 * GET/POST /console, protected by basic auth):
 *   - the port listens for LOG_SERVER_WINDOW_S seconds waiting for a client;
 *   - when a client connects the port is closed again (single client);
 *   - when the client disconnects the session ends and the port stays closed;
 *   - if nobody connects within the window the port is closed.
 * Connect with e.g. `nc <ip> 2323` or `telnet <ip> 2323`. The most recent log
 * lines (RAM ring buffer) are sent first, then new lines are streamed.
 * Input from the client is ignored.
 */

#define LOG_SERVER_PORT 2323
#define LOG_SERVER_WINDOW_S 30

typedef enum
{
    LOG_OPEN_STARTED,  /* port opened now */
    LOG_OPEN_EXTENDED, /* already waiting for a client: window restarted */
    LOG_OPEN_BUSY,     /* a client is connected right now */
    LOG_OPEN_ERROR,    /* log server task not running */
} log_open_result_t;

/* Capture ESP log output into the ring buffer (still forwarded to UART).
 * Call as early as possible in app_main() to also capture boot logs.
 * Needs no network and does not allocate. */
void log_server_install_hook(void);

/* Create the (idle) log server task. Call after esp_netif_init(). No port is opened. */
void log_server_start(void);

/* Open the port for LOG_SERVER_WINDOW_S seconds (see above). */
log_open_result_t log_server_request_open(void);

#endif // LOG_SERVER_H
