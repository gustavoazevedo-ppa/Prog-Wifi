#ifndef LOG_SERVER_H
#define LOG_SERVER_H

/**
 * Read-only remote log console (telnet style raw TCP, port 2323).
 *
 * Connect with e.g. `nc <ip> 2323` or `telnet <ip> 2323`. The most recent
 * log lines (from a small RAM ring buffer) are sent first, then new lines are
 * streamed as they are logged. One client at a time: a new connection
 * replaces the previous one. Input from the client is ignored.
 */

/* Capture ESP log output into the ring buffer (still forwarded to UART).
 * Call as early as possible in app_main() to also capture boot logs.
 * Needs no network and does not allocate. */
void log_server_install_hook(void);

/* Start the TCP listener task. Call after esp_netif_init(). */
void log_server_start(void);

#endif // LOG_SERVER_H
