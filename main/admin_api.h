#ifndef ADMIN_API_H
#define ADMIN_API_H

#include <stdbool.h>
#include "esp_http_server.h"

/**
 * Field-maintenance HTTP endpoints (HTTP Basic auth, same credentials as /console).
 *
 *   GET  /admin/status              state of the current/last operation (JSON)
 *   POST /admin/update/check        check for a newer firmware   (?channel=beta)
 *   POST /admin/update/apply        download + flash it, no reboot (?channel=beta)
 *   POST /admin/format?confirm=yes  erase NVS, then reboot
 *   POST /admin/reboot              reboot
 *   GET  /admin/history             last 20 POST /iniciar requests, newest first (JSON)
 *   GET  /admin/preset              last successfully decoded preset: metadata + hex (JSON);
 *                                   ?format=text returns only the hex string
 *
 * Every POST also needs the header "X-Admin: 1". A browser will not add a custom
 * header on a cross-site request without a CORS preflight, which the device never
 * answers, so a malicious page cannot reuse the browser's cached Basic credentials.
 *
 * Operations are refused with 409 while a test run is in progress or while another
 * admin operation is running. Check/apply run in a temporary task and are polled
 * through GET /admin/status.
 *
 * Call once, after the httpd server has been started.
 */
void admin_api_register(httpd_handle_t server);

/**
 * True while a firmware update download, an NVS format or a reboot is in progress.
 * POST /iniciar is refused (409) meanwhile. An update *check* does not block runs.
 * Takes the admin lock briefly; never call it while holding that lock.
 */
bool admin_api_blocks_runs(void);

#endif // ADMIN_API_H
