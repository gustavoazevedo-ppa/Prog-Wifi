/****************************************************************************
 * Field-maintenance HTTP endpoints: update check/apply, NVS format, reboot.
 * See admin_api.h for the endpoint list and the safety rules.
 ****************************************************************************/

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "admin_api.h"
#include "ota_button.h"
#include "preset_server.h"

#define TAG "ADMIN"

#define ADMIN_JOB_STACK 10240   /* TLS client (manifest + image download) */
#define ADMIN_DEFER_MS 800      /* let the HTTP reply leave before NVS erase / reboot */
#define VER_LEN 24
#define MSG_LEN 96

typedef enum
{
    OP_IDLE = 0,
    OP_CHECKING,
    OP_UPDATING,
    OP_FORMATTING,
    OP_REBOOTING,
} admin_op_t;

static const char *const OP_NAME[] = {"idle", "checking", "updating", "formatting", "rebooting"};

typedef struct
{
    admin_op_t op;
    volatile uint8_t progress; /* 0-100 while updating */
    char last_op[12];          /* "check", "update", "format" */
    bool last_ok;
    char last_msg[MSG_LEN];
    char remote[VER_LEN];      /* version seen in the last manifest ("" = unknown) */
    bool update_available;
    bool staged;               /* a new image is written, takes effect after reboot */
    char staged_ver[VER_LEN];
} admin_state_t;

static admin_state_t s_st;
static SemaphoreHandle_t s_lock;

/* Parameters handed to the temporary tasks (only one runs at a time: guarded by s_st.op) */
static admin_op_t s_job_op;
static const char *s_job_url;

/* ─── Helpers ────────────────────────────────────────────────────────────── */

/* Keep only characters that are safe inside a JSON string and a version number. */
static void copy_safe(char *dst, size_t dst_len, const char *src)
{
    size_t n = 0;
    for (; src != NULL && *src != '\0' && n + 1 < dst_len; src++)
    {
        if (isalnum((unsigned char)*src) || *src == '.' || *src == '-' || *src == '+' || *src == '_')
        {
            dst[n++] = *src;
        }
    }
    dst[n] = '\0';
}

static bool begin_op(admin_op_t op)
{
    bool ok;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    ok = (s_st.op == OP_IDLE);
    if (ok)
    {
        s_st.op = op;
        s_st.progress = 0;
    }
    xSemaphoreGive(s_lock);
    return ok;
}

/* Finish the current operation and record its outcome. */
static void end_op(const char *last_op, bool ok, const char *fmt, const char *arg)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_st.last_op, last_op, sizeof(s_st.last_op));
    s_st.last_ok = ok;
    snprintf(s_st.last_msg, sizeof(s_st.last_msg), fmt, arg);
    s_st.op = OP_IDLE;
    xSemaphoreGive(s_lock);
}

static esp_err_t reply(httpd_req_t *req, const char *status, bool ok, const char *msg)
{
    char buf[160];
    snprintf(buf, sizeof(buf), "{\"ok\":%s,\"message\":\"%s\"}", ok ? "true" : "false", msg);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t reply_unauthorized(httpd_req_t *req)
{
    ESP_LOGW(TAG, "%s %s rejected (401): bad or missing basic auth",
             req->method == HTTP_POST ? "POST" : "GET", req->uri);
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"Prog\"");
    return httpd_resp_send(req, NULL, 0);
}

/* Auth + CSRF header. Returns false after sending the error reply. */
static bool guard_post(httpd_req_t *req)
{
    if (!preset_server_basic_auth_ok(req))
    {
        reply_unauthorized(req);
        return false;
    }
    if (httpd_req_get_hdr_value_len(req, "X-Admin") == 0)
    {
        ESP_LOGW(TAG, "POST %s rejected (403): missing X-Admin header", req->uri);
        reply(req, "403 Forbidden", false, "Missing X-Admin header");
        return false;
    }
    return true;
}

/* Common pre-conditions for every state-changing operation. Returns false after replying.
 * Order matters: take the admin slot first, then look at the test run. POST /iniciar does
 * the opposite check inside its own critical section (state_try_begin), so one of the two
 * always sees the other and a test run and an update can never start together. */
static bool claim(httpd_req_t *req, admin_op_t op)
{
    if (!begin_op(op))
    {
        ESP_LOGW(TAG, "POST %s rejected (409): another admin operation is running", req->uri);
        reply(req, "409 Conflict", false, "Another admin operation is in progress");
        return false;
    }
    if (preset_server_run_active())
    {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_st.op = OP_IDLE; /* give the slot back, nothing was done */
        xSemaphoreGive(s_lock);
        ESP_LOGW(TAG, "POST %s rejected (409): test run in progress", req->uri);
        reply(req, "409 Conflict", false, "Test run in progress");
        return false;
    }
    return true;
}

bool admin_api_blocks_runs(void)
{
    bool blocks;
    if (s_lock == NULL)
    {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    blocks = (s_st.op == OP_UPDATING || s_st.op == OP_FORMATTING || s_st.op == OP_REBOOTING);
    xSemaphoreGive(s_lock);
    return blocks;
}

static const char *manifest_url_from_query(httpd_req_t *req)
{
    char q[64];
    char v[16];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
        httpd_query_key_value(q, "channel", v, sizeof(v)) == ESP_OK && strcmp(v, "beta") == 0)
    {
        return OTA_MANIFEST_URL_BETA;
    }
    return OTA_MANIFEST_URL;
}

/* ─── Temporary tasks ────────────────────────────────────────────────────── */

/* Update check / apply: blocking TLS work, kept off the httpd task. */
static void job_task(void *arg)
{
    const admin_op_t op = s_job_op;
    char remote[VER_LEN] = "";
    char remote_raw[VER_LEN] = "";
    esp_err_t err;

    if (op == OP_CHECKING)
    {
        err = ota_check_update(s_job_url, remote_raw, sizeof(remote_raw));
    }
    else
    {
        err = ota_apply_update(s_job_url, &s_st.progress, remote_raw, sizeof(remote_raw));
    }
    copy_safe(remote, sizeof(remote), remote_raw);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_st.remote, remote, sizeof(s_st.remote));
    if (op == OP_CHECKING)
    {
        s_st.update_available = (err == ESP_OK);
    }
    else if (err == ESP_OK)
    {
        s_st.staged = true;
        s_st.update_available = false;
        strlcpy(s_st.staged_ver, remote, sizeof(s_st.staged_ver));
    }
    xSemaphoreGive(s_lock);

    const char *name = (op == OP_CHECKING) ? "check" : "update";
    if (err == ESP_OK)
    {
        end_op(name, true, (op == OP_CHECKING) ? "Update available: %s" : "Update %s installed. Reboot to apply.",
               remote);
    }
    else if (err == ESP_ERR_NOT_FOUND)
    {
        end_op(name, true, "Already up to date (remote %s)", remote);
    }
    else
    {
        end_op(name, false, "Failed: %s", esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "%s finished: %s", name, esp_err_to_name(err));
    vTaskDelete(NULL);
}

/* Format / reboot: wait for the HTTP reply to be sent, then act. */
static void deferred_task(void *arg)
{
    const admin_op_t op = s_job_op;
    vTaskDelay(pdMS_TO_TICKS(ADMIN_DEFER_MS));

    if (op == OP_FORMATTING)
    {
        ESP_LOGW(TAG, "Erasing NVS");
        esp_err_t err = ota_format_nvs();
        if (err != ESP_OK)
        {
            end_op("format", false, "NVS erase failed: %s", esp_err_to_name(err));
            vTaskDelete(NULL);
            return;
        }
    }
    ESP_LOGW(TAG, "Restarting");
    esp_restart();
}

/* ─── Handlers ───────────────────────────────────────────────────────────── */

static esp_err_t start_job(httpd_req_t *req, admin_op_t op)
{
    if (!guard_post(req) || !claim(req, op))
    {
        return ESP_OK;
    }
    s_job_op = op;
    s_job_url = manifest_url_from_query(req);
    if (xTaskCreate(job_task, "admin_job", ADMIN_JOB_STACK, NULL, 5, NULL) != pdPASS)
    {
        end_op((op == OP_CHECKING) ? "check" : "update", false, "%s", "Not enough memory to start");
        return reply(req, "500 Internal Server Error", false, "Not enough memory to start the job");
    }
    ESP_LOGI(TAG, "%s started (%s)", OP_NAME[op], s_job_url);
    return reply(req, "202 Accepted", true, (op == OP_CHECKING) ? "Checking for updates" : "Updating firmware");
}

static esp_err_t check_handler(httpd_req_t *req)
{
    return start_job(req, OP_CHECKING);
}

static esp_err_t apply_handler(httpd_req_t *req)
{
    return start_job(req, OP_UPDATING);
}

static esp_err_t start_deferred(httpd_req_t *req, admin_op_t op, const char *msg)
{
    s_job_op = op;
    if (xTaskCreate(deferred_task, "admin_defer", 4096, NULL, 5, NULL) != pdPASS)
    {
        end_op("reboot", false, "%s", "Not enough memory to start");
        return reply(req, "500 Internal Server Error", false, "Not enough memory");
    }
    ESP_LOGW(TAG, "%s requested", OP_NAME[op]);
    return reply(req, "200 OK", true, msg);
}

static esp_err_t format_handler(httpd_req_t *req)
{
    if (!guard_post(req))
    {
        return ESP_OK;
    }
    char q[64];
    char v[8];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK ||
        httpd_query_key_value(q, "confirm", v, sizeof(v)) != ESP_OK || strcmp(v, "yes") != 0)
    {
        return reply(req, "400 Bad Request", false, "Add ?confirm=yes to erase the settings");
    }
    if (!claim(req, OP_FORMATTING))
    {
        return ESP_OK;
    }
    return start_deferred(req, OP_FORMATTING, "Erasing settings and restarting");
}

static esp_err_t reboot_handler(httpd_req_t *req)
{
    if (!guard_post(req) || !claim(req, OP_REBOOTING))
    {
        return ESP_OK;
    }
    return start_deferred(req, OP_REBOOTING, "Restarting");
}

static esp_err_t status_handler(httpd_req_t *req)
{
    if (!preset_server_basic_auth_ok(req))
    {
        return reply_unauthorized(req);
    }

    admin_state_t st;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    st = s_st;
    xSemaphoreGive(s_lock);

    char running[VER_LEN];
    copy_safe(running, sizeof(running), esp_app_get_description()->version);

    char buf[448];
    snprintf(buf, sizeof(buf),
             "{\"op\":\"%s\",\"progress\":%u,\"running\":\"%s\",\"remote\":\"%s\","
             "\"update_available\":%s,\"staged\":%s,\"staged_version\":\"%s\",\"test_running\":%s,"
             "\"last\":{\"op\":\"%s\",\"ok\":%s,\"message\":\"%s\"}}",
             OP_NAME[st.op], (unsigned)st.progress, running, st.remote,
             st.update_available ? "true" : "false", st.staged ? "true" : "false", st.staged_ver,
             preset_server_run_active() ? "true" : "false",
             st.last_op, st.last_ok ? "true" : "false", st.last_msg);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

/* ─── Registration ───────────────────────────────────────────────────────── */

void admin_api_register(httpd_handle_t server)
{
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL)
    {
        ESP_LOGE(TAG, "mutex creation failed: admin API disabled");
        return;
    }

    static const httpd_uri_t uris[] = {
        {.uri = "/admin/status", .method = HTTP_GET, .handler = status_handler},
        {.uri = "/admin/update/check", .method = HTTP_POST, .handler = check_handler},
        {.uri = "/admin/update/apply", .method = HTTP_POST, .handler = apply_handler},
        {.uri = "/admin/format", .method = HTTP_POST, .handler = format_handler},
        {.uri = "/admin/reboot", .method = HTTP_POST, .handler = reboot_handler},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++)
    {
        if (httpd_register_uri_handler(server, &uris[i]) != ESP_OK)
        {
            ESP_LOGE(TAG, "failed to register %s", uris[i].uri);
        }
    }
    ESP_LOGI(TAG, "admin API ready (/admin/status, update/check, update/apply, format, reboot)");
}
