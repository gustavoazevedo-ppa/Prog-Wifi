/****************************************************************************
 * One log line per HTTP request. See http_log.h.
 ****************************************************************************/

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "http_log.h"

#define TAG "HTTP"

#define HTTP_LOG_MAX_URIS 24 /* wrapped handlers (13 used today) */
#define URI_LOG_LEN 96       /* longest URI printed */

typedef struct
{
    esp_err_t (*fn)(httpd_req_t *req);
    void *ctx;
} wrapped_t;

static wrapped_t s_wrapped[HTTP_LOG_MAX_URIS];
static size_t s_wrapped_n;

/* State of the request the httpd task is handling right now. esp_http_server runs handlers
 * one at a time in a single task, so plain statics are enough. Other tasks (the async
 * reply of POST /iniciar) must not touch it: note_status() ignores them. */
static TaskHandle_t s_httpd_task;
static int s_status;
static int64_t s_t0_us;
static bool s_async;

static void note_status(int code)
{
    if (xTaskGetCurrentTaskHandle() == s_httpd_task)
    {
        s_status = code;
    }
}

static int err_code_to_int(httpd_err_code_t c)
{
    switch (c)
    {
    case HTTPD_400_BAD_REQUEST: return 400;
    case HTTPD_404_NOT_FOUND: return 404;
    case HTTPD_405_METHOD_NOT_ALLOWED: return 405;
    case HTTPD_408_REQ_TIMEOUT: return 408;
    case HTTPD_411_LENGTH_REQUIRED: return 411;
    case HTTPD_414_URI_TOO_LONG: return 414;
    case HTTPD_431_REQ_HDR_FIELDS_TOO_LARGE: return 431;
    case HTTPD_501_METHOD_NOT_IMPLEMENTED: return 501;
    case HTTPD_505_VERSION_NOT_SUPPORTED: return 505;
    default: return 500;
    }
}

/* Client address. The server socket is dual stack, so IPv4 clients show up as
 * IPv4-mapped IPv6 addresses: print those as plain dotted IPv4. */
static void peer_str(httpd_req_t *req, char *out, size_t n)
{
    struct sockaddr_storage ss;
    socklen_t len = sizeof(ss);
    int fd = httpd_req_to_sockfd(req);
    if (fd < 0 || getpeername(fd, (struct sockaddr *)&ss, &len) != 0)
    {
        snprintf(out, n, "?");
        return;
    }
    if (ss.ss_family == AF_INET)
    {
        const struct sockaddr_in *a = (const struct sockaddr_in *)&ss;
        if (inet_ntop(AF_INET, &a->sin_addr, out, (socklen_t)n) == NULL)
        {
            snprintf(out, n, "?");
        }
    }
    else if (ss.ss_family == AF_INET6)
    {
        const struct sockaddr_in6 *a6 = (const struct sockaddr_in6 *)&ss;
        const uint8_t *b = (const uint8_t *)&a6->sin6_addr;
        static const uint8_t mapped_prefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
        if (memcmp(b, mapped_prefix, sizeof(mapped_prefix)) == 0)
        {
            snprintf(out, n, "%u.%u.%u.%u", b[12], b[13], b[14], b[15]);
        }
        else if (inet_ntop(AF_INET6, &a6->sin6_addr, out, (socklen_t)n) == NULL)
        {
            snprintf(out, n, "?");
        }
    }
    else
    {
        snprintf(out, n, "?");
    }
}

static void log_line(httpd_req_t *req, int status, int64_t t0_us)
{
    char ip[48];
    char uri[URI_LOG_LEN];
    peer_str(req, ip, sizeof(ip));

    /* The URI comes from the client: keep the log line printable and bounded. */
    size_t i = 0;
    for (; req->uri[i] != '\0' && i + 1 < sizeof(uri); i++)
    {
        unsigned char c = (unsigned char)req->uri[i];
        uri[i] = (c >= 0x20 && c <= 0x7E) ? (char)c : '?';
    }
    uri[i] = '\0';

    const char *method = http_method_str((enum http_method)req->method);
    esp_log_level_t level = (status >= 400) ? ESP_LOG_WARN : ESP_LOG_INFO;
    if (t0_us > 0)
    {
        ESP_LOG_LEVEL(level, TAG, "%s %s %s -> %d (%lld ms)", ip, method ? method : "?", uri, status,
                      (long long)((esp_timer_get_time() - t0_us) / 1000));
    }
    else
    {
        ESP_LOG_LEVEL(level, TAG, "%s %s %s -> %d", ip, method ? method : "?", uri, status);
    }
}

/* ─── Handler wrapper ────────────────────────────────────────────────────── */
static esp_err_t logged_handler(httpd_req_t *req)
{
    const wrapped_t *w = &s_wrapped[(size_t)(uintptr_t)req->user_ctx];

    s_httpd_task = xTaskGetCurrentTaskHandle();
    s_status = 200; /* what esp_http_server sends when the handler never sets a status */
    s_async = false;
    s_t0_us = esp_timer_get_time();
    const int64_t t0 = s_t0_us;

    req->user_ctx = w->ctx; /* the handler sees its own context, not our index */
    esp_err_t err = w->fn(req);

    if (!s_async)
    {
        if (err != ESP_OK && s_status < 400)
        {
            s_status = 500; /* handler failed before replying: the connection is closed */
        }
        log_line(req, s_status, t0);
    }
    return err;
}

esp_err_t http_log_register(httpd_handle_t server, const httpd_uri_t *uri)
{
    if (s_wrapped_n >= HTTP_LOG_MAX_URIS)
    {
        ESP_LOGE(TAG, "too many handlers, %s registered without logging", uri->uri);
        return httpd_register_uri_handler(server, uri);
    }
    s_wrapped[s_wrapped_n].fn = uri->handler;
    s_wrapped[s_wrapped_n].ctx = uri->user_ctx;

    httpd_uri_t wrapped = *uri;
    wrapped.handler = logged_handler;
    wrapped.user_ctx = (void *)(uintptr_t)s_wrapped_n;
    esp_err_t err = httpd_register_uri_handler(server, &wrapped);
    if (err == ESP_OK)
    {
        s_wrapped_n++;
    }
    return err;
}

/* ─── 404 / 405: requests that never reach a handler ─────────────────────── */
static esp_err_t error_handler(httpd_req_t *req, httpd_err_code_t error)
{
    log_line(req, err_code_to_int(error), 0);
    return httpd_resp_send_err(req, error, NULL); /* the standard reply */
}

void http_log_install_error_handlers(httpd_handle_t server)
{
    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, error_handler);
    httpd_register_err_handler(server, HTTPD_405_METHOD_NOT_ALLOWED, error_handler);
}

/* ─── Status capture ─────────────────────────────────────────────────────── */
esp_err_t http_log_set_status(httpd_req_t *req, const char *status)
{
    note_status(atoi(status));
    return httpd_resp_set_status(req, status);
}

esp_err_t http_log_send_err(httpd_req_t *req, httpd_err_code_t code, const char *msg)
{
    note_status(err_code_to_int(code));
    return httpd_resp_send_err(req, code, msg);
}

/* ─── Async requests ─────────────────────────────────────────────────────── */
int64_t http_log_request_start_us(void)
{
    return s_t0_us;
}

void http_log_async_detach(void)
{
    s_async = true;
}

void http_log_async_done(httpd_req_t *req, const char *status, int64_t t0_us)
{
    log_line(req, atoi(status), t0_us);
}
