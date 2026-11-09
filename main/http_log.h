#ifndef HTTP_LOG_H
#define HTTP_LOG_H

#include <stdint.h>
#include "esp_err.h"
#include "esp_http_server.h"

/**
 * One log line (tag "HTTP", level INFO) for every request the HTTP server handles:
 *
 *   I (123456) HTTP: 192.168.1.20 GET /status -> 200 (3 ms)
 *   W (123789) HTTP: 192.168.1.20 POST /iniciar -> 401 (1 ms)
 *   I (124001) HTTP: 192.168.1.20 GET /nope -> 404
 *
 * Client address, method, URI (with query, never headers or bodies), HTTP status and
 * time spent. Status >= 400 is logged as a warning. The lines go to the UART and to the
 * remote log console like every other log. To silence them at run time:
 *   esp_log_level_set("HTTP", ESP_LOG_WARN);
 *
 * How it works:
 *  - http_log_register() replaces httpd_register_uri_handler(): it wraps the handler and
 *    logs after it returns.
 *  - http_log_install_error_handlers() covers requests that never reach a handler (404, 405).
 *  - The status can only be known if the handler sets it through http_log_set_status() /
 *    http_log_send_err() instead of the httpd_resp_* functions. A handler that sets nothing
 *    is logged as 200.
 *  - A request answered later by another task (esp_http_server async API) is logged when
 *    that task calls http_log_async_done().
 */

/* Drop-in replacement for httpd_register_uri_handler(). */
esp_err_t http_log_register(httpd_handle_t server, const httpd_uri_t *uri);

/* Log 404 / 405 answers (the standard reply is still sent). Call once after httpd_start(). */
void http_log_install_error_handlers(httpd_handle_t server);

/* Drop-in replacement for httpd_resp_set_status(): also remembers the code for the log. */
esp_err_t http_log_set_status(httpd_req_t *req, const char *status);

/* Drop-in replacement for httpd_resp_send_err(): also remembers the code for the log. */
esp_err_t http_log_send_err(httpd_req_t *req, httpd_err_code_t code, const char *msg);

/* Async requests. Inside the handler, after the work was handed to another task:
 *   job->t0_us = http_log_request_start_us();   (before handing over)
 *   ... handed over successfully ...
 *   http_log_async_detach();                    (the handler wrapper then does not log)
 * and in the task that sends the reply:
 *   http_log_async_done(async_req, "200 OK", job->t0_us);   (before completing the request) */
int64_t http_log_request_start_us(void);
void http_log_async_detach(void);
void http_log_async_done(httpd_req_t *req, const char *status, int64_t t0_us);

#endif // HTTP_LOG_H
