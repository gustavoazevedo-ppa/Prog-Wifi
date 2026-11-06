/****************************************************************************
 * Preset / test-start HTTP API (see docs/preset-api-spec.md)
 *
 *   POST /iniciar : resolve + download exemplo.conf, "send" it to the DUT
 *                   (logged only for now) and start a simulated test run.
 *   GET  /status  : {"id":N,"status":0..3,"message":[...]}
 ****************************************************************************/

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_server.h"
#include "esp_http_client.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "device_info.h"
#include "ethernet_manager.h"
#include "wifi_manager.h"
#include "mbedtls/base64.h"
#include "log_server.h"
#include "preset_server.h"

#define TAG "PRESET"

/* ─── Inline constants (phase 1) ─────────────────────────────────────────── */
#define PRESET_API_KEY "change-me-prog-key"        /* expected X-API-Key value */
#define CONSOLE_AUTH_USER "admin"                  /* basic auth for /console */
#define CONSOLE_AUTH_PASS "change-me-console"
#define PRESET_FILE_NAME "exemplo.conf"
#define PRESET_SIM_RUN_MS 5000                     /* simulated run: status 1 -> 2 */
#define PRESET_SIM_ACTION_TEXT "UART: send preset to DUT and start tests (simulated)"
#define PRESET_UART_BAUD 9600                      /* used to emulate TX time */
#define PRESET_MAX_HEX_CHARS 6144                  /* 2 KB binary = 4096 hex chars, plus slack */
#define PRESET_FETCH_TIMEOUT_MS 5000
#define PRESET_MAX_BODY 1024                       /* max POST body size */
#define PRESET_URL_LEN 448
#define PRESET_BASE_LEN 240                        /* caminho buffer */
#define PRESET_FIELD_LEN 160
#define PRESET_MAX_MSGS 4
#define PRESET_MSG_LEN 160
#define PRESET_SERVER_PORT 80

/* ─── Run state ──────────────────────────────────────────────────────────── */
enum
{
    ST_IDLE = 0,
    ST_RUNNING = 1,
    ST_OK = 2,
    ST_ERROR = 3,
};

typedef struct
{
    bool id_is_string;
    double id_num;
    char id_str[32];
    int status;
    int msg_count;
    char msg[PRESET_MAX_MSGS][PRESET_MSG_LEN];
} run_state_t;

static run_state_t s_state;
static SemaphoreHandle_t s_lock;
static esp_timer_handle_t s_sim_timer;
static httpd_handle_t s_server;

static void state_snapshot(run_state_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_state;
    xSemaphoreGive(s_lock);
}

static void state_commit(const run_state_t *in)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state = *in;
    xSemaphoreGive(s_lock);
}

/* Atomically start a run: fails (and returns the current state in *cur) if one is active. */
static bool state_try_begin(const run_state_t *in, run_state_t *cur)
{
    bool ok;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_state.status == ST_RUNNING)
    {
        *cur = s_state;
        ok = false;
    }
    else
    {
        s_state = *in;
        ok = true;
    }
    xSemaphoreGive(s_lock);
    return ok;
}

/* Work item handed from the HTTP task to the worker task */
typedef struct
{
    httpd_req_t *req; /* async copy of the request, completed by the worker */
    run_state_t st;
    char cliente[PRESET_FIELD_LEN];
    char estado[PRESET_FIELD_LEN];
    char caminho[PRESET_BASE_LEN];
} job_t;

static QueueHandle_t s_job_queue;

/* Append a message (ASCII only: anything outside 0x20..0x7E becomes '?'). */
static void add_msg(run_state_t *st, const char *fmt, ...)
{
    if (st->msg_count >= PRESET_MAX_MSGS)
    {
        return;
    }
    char *dst = st->msg[st->msg_count++];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(dst, PRESET_MSG_LEN, fmt, ap);
    va_end(ap);
    for (char *p = dst; *p; p++)
    {
        if ((unsigned char)*p < 0x20 || (unsigned char)*p > 0x7E)
        {
            *p = '?';
        }
    }
}

/* ─── Responses ──────────────────────────────────────────────────────────── */
static esp_err_t send_status(httpd_req_t *req, const char *http_status, const run_state_t *st)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *msgs = cJSON_CreateArray();
    if (root == NULL || msgs == NULL)
    {
        cJSON_Delete(root);
        cJSON_Delete(msgs);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }

    if (st->id_is_string)
    {
        cJSON_AddStringToObject(root, "id", st->id_str);
    }
    else
    {
        cJSON_AddNumberToObject(root, "id", st->id_num);
    }
    cJSON_AddNumberToObject(root, "status", st->status);
    for (int i = 0; i < st->msg_count; i++)
    {
        cJSON_AddItemToArray(msgs, cJSON_CreateString(st->msg[i]));
    }
    cJSON_AddItemToObject(root, "message", msgs);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }

    httpd_resp_set_status(req, http_status);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    free(json);
    return err;
}

/* 400 reply: status object (status 3) that does NOT touch the stored run state. */
static esp_err_t send_bad_request(httpd_req_t *req, const run_state_t *id_src, const char *reason)
{
    run_state_t st = {0};
    if (id_src != NULL)
    {
        st.id_is_string = id_src->id_is_string;
        st.id_num = id_src->id_num;
        memcpy(st.id_str, id_src->id_str, sizeof(st.id_str));
    }
    st.status = ST_ERROR;
    add_msg(&st, "%s", reason);
    ESP_LOGW(TAG, "POST /iniciar rejected (400): %s", reason);
    return send_status(req, "400 Bad Request", &st);
}

static esp_err_t send_unauthorized(httpd_req_t *req)
{
    ESP_LOGW(TAG, "%s %s rejected (401): missing or wrong X-API-Key",
             req->method == HTTP_POST ? "POST" : "GET", req->uri);
    httpd_resp_set_status(req, "401 Unauthorized");
    return httpd_resp_send(req, NULL, 0);
}

/* ─── Auth ───────────────────────────────────────────────────────────────── */
static bool auth_ok(httpd_req_t *req)
{
    char key[64];
    size_t len = httpd_req_get_hdr_value_len(req, "X-API-Key");
    if (len == 0 || len >= sizeof(key))
    {
        return false;
    }
    if (httpd_req_get_hdr_value_str(req, "X-API-Key", key, sizeof(key)) != ESP_OK)
    {
        return false;
    }
    /* Constant-time compare */
    const char *expected = PRESET_API_KEY;
    size_t elen = strlen(expected);
    if (strlen(key) != elen)
    {
        return false;
    }
    unsigned char diff = 0;
    for (size_t i = 0; i < elen; i++)
    {
        diff |= (unsigned char)(key[i] ^ expected[i]);
    }
    return diff == 0;
}

/* ─── Small helpers ──────────────────────────────────────────────────────── */
static char *trim(char *s)
{
    while (*s && isspace((unsigned char)*s))
    {
        s++;
    }
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1]))
    {
        s[--n] = '\0';
    }
    return s;
}

/* Copy an optional string (or number) JSON field into out, trimmed. */
static void get_str_field(const cJSON *root, const char *name, char *out, size_t out_len)
{
    out[0] = '\0';
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    char tmp[PRESET_BASE_LEN];
    if (cJSON_IsString(item) && item->valuestring != NULL)
    {
        snprintf(tmp, sizeof(tmp), "%s", item->valuestring);
    }
    else if (cJSON_IsNumber(item))
    {
        snprintf(tmp, sizeof(tmp), "%.0f", item->valuedouble);
    }
    else
    {
        return;
    }
    snprintf(out, out_len, "%s", trim(tmp));
}

static bool segment_unsafe(const char *s)
{
    return strchr(s, '/') != NULL || strchr(s, '\\') != NULL || strstr(s, "..") != NULL;
}

static int read_body(httpd_req_t *req, char *buf, size_t cap)
{
    size_t total = req->content_len;
    if (total == 0 || total >= cap)
    {
        return -1;
    }
    size_t got = 0;
    int timeouts = 0;
    while (got < total)
    {
        int r = httpd_req_recv(req, buf + got, total - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT)
        {
            if (++timeouts > 3)
            {
                return -1;
            }
            continue;
        }
        if (r <= 0)
        {
            return -1;
        }
        got += (size_t)r;
    }
    buf[got] = '\0';
    return (int)got;
}

/* ─── Preset download ────────────────────────────────────────────────────── */
typedef struct
{
    char *buf;
    size_t len;
    size_t cap;
    bool overflow;
} fetch_ctx_t;

static esp_err_t fetch_evt_handler(esp_http_client_event_t *evt)
{
    fetch_ctx_t *c = (fetch_ctx_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && c != NULL &&
        esp_http_client_get_status_code(evt->client) == 200)
    {
        if (c->len + (size_t)evt->data_len > c->cap)
        {
            c->overflow = true;
            return ESP_OK;
        }
        memcpy(c->buf + c->len, evt->data, (size_t)evt->data_len);
        c->len += (size_t)evt->data_len;
    }
    return ESP_OK;
}

/* Returns the HTTP status code (>0) or -1 on transport error (err_out set). */
static int fetch_preset(const char *url, fetch_ctx_t *ctx, esp_err_t *err_out)
{
    ctx->len = 0;
    ctx->overflow = false;

    esp_http_client_config_t cfg = {
        .url = url,
        .event_handler = fetch_evt_handler,
        .user_data = ctx,
        .timeout_ms = PRESET_FETCH_TIMEOUT_MS,
        .buffer_size = 1024,
        .buffer_size_tx = 512,
        .skip_cert_common_name_check = true, /* phase 1: self-signed CA, no verification */
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL)
    {
        *err_out = ESP_ERR_NO_MEM;
        return -1;
    }
    esp_err_t err = esp_http_client_perform(client);
    int code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK)
    {
        *err_out = err;
        return -1;
    }
    return code;
}

/* Strip BOM/whitespace in place, validate hex, decode into bin. Returns byte count or -1. */
static int hex_decode(char *text, size_t len, uint8_t *bin, size_t bin_cap, char *clean_out)
{
    size_t n = 0;
    size_t i = 0;
    if (len >= 3 && (uint8_t)text[0] == 0xEF && (uint8_t)text[1] == 0xBB && (uint8_t)text[2] == 0xBF)
    {
        i = 3;
    }
    for (; i < len; i++)
    {
        if (isspace((unsigned char)text[i]))
        {
            continue;
        }
        if (!isxdigit((unsigned char)text[i]))
        {
            return -1;
        }
        clean_out[n++] = text[i];
    }
    clean_out[n] = '\0';
    if (n == 0 || (n % 2) != 0 || (n / 2) > bin_cap)
    {
        return -1;
    }
    for (size_t k = 0; k < n; k += 2)
    {
        char pair[3] = {clean_out[k], clean_out[k + 1], 0};
        bin[k / 2] = (uint8_t)strtoul(pair, NULL, 16);
    }
    return (int)(n / 2);
}

/* ─── Simulated run ──────────────────────────────────────────────────────── */
static void sim_run_done_cb(void *arg)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool finished = false;
    if (s_state.status == ST_RUNNING)
    {
        s_state.status = ST_OK;
        finished = true;
    }
    xSemaphoreGive(s_lock);
    if (finished)
    {
        ESP_LOGI(TAG, "Simulated run finished: status 2 (OK)");
    }
}

/* ─── POST /iniciar ──────────────────────────────────────────────────────── */
static esp_err_t iniciar_post_handler(httpd_req_t *req)
{
    if (!auth_ok(req))
    {
        return send_unauthorized(req);
    }

    /* --- Parse body --- */
    char body[PRESET_MAX_BODY];
    run_state_t st = {0};
    if (read_body(req, body, sizeof(body)) < 0)
    {
        return send_bad_request(req, NULL, "empty, unreadable or too large body");
    }
    cJSON *root = cJSON_Parse(body);
    if (root == NULL || !cJSON_IsObject(root))
    {
        cJSON_Delete(root);
        return send_bad_request(req, NULL, "invalid JSON");
    }

    /* id: number or numeric string (echoed back with the same type) */
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
    if (cJSON_IsNumber(id))
    {
        st.id_num = id->valuedouble;
    }
    else if (cJSON_IsString(id) && id->valuestring != NULL)
    {
        snprintf(st.id_str, sizeof(st.id_str), "%s", id->valuestring);
        char *t = trim(st.id_str);
        char *end = NULL;
        st.id_num = strtod(t, &end);
        if (*t == '\0' || end == NULL || *end != '\0')
        {
            cJSON_Delete(root);
            st.id_str[0] = '\0';
            return send_bad_request(req, &st, "id must be a number or numeric string");
        }
        memmove(st.id_str, t, strlen(t) + 1);
        st.id_is_string = true;
    }
    else
    {
        cJSON_Delete(root);
        return send_bad_request(req, NULL, "missing id");
    }

    char produto[PRESET_FIELD_LEN], cliente[PRESET_FIELD_LEN], estado[PRESET_FIELD_LEN];
    char caminho[PRESET_BASE_LEN];
    get_str_field(root, "produto", produto, sizeof(produto));
    get_str_field(root, "cliente", cliente, sizeof(cliente));
    get_str_field(root, "estado", estado, sizeof(estado));
    get_str_field(root, "caminho", caminho, sizeof(caminho));
    cJSON_Delete(root);

    if (caminho[0] == '\0')
    {
        return send_bad_request(req, &st, "missing caminho");
    }
    if (strncasecmp(caminho, "http://", 7) != 0 && strncasecmp(caminho, "https://", 8) != 0)
    {
        return send_bad_request(req, &st, "caminho must start with http:// or https://");
    }
    if (segment_unsafe(cliente) || segment_unsafe(estado))
    {
        return send_bad_request(req, &st, "cliente/estado contain invalid characters");
    }
    size_t clen = strlen(caminho);
    if (caminho[clen - 1] != '/')
    {
        if (clen + 1 >= sizeof(caminho))
        {
            return send_bad_request(req, &st, "caminho too long");
        }
        caminho[clen] = '/';
        caminho[clen + 1] = '\0';
    }

    /* --- Busy check + accept (atomic): the run is "running" until the preset is handled --- */
    st.status = ST_RUNNING;
    run_state_t cur;
    if (!state_try_begin(&st, &cur))
    {
        ESP_LOGW(TAG, "POST /iniciar rejected (409): a run is already in progress");
        return send_status(req, "409 Conflict", &cur);
    }

    ESP_LOGI(TAG, "POST /iniciar id=%.0f produto='%s' cliente='%s' estado='%s' caminho='%s'",
             st.id_num, produto, cliente, estado, caminho);

    /* --- Hand the request to the worker so the HTTP task stays free (/status, 409) --- */
    job_t *job = calloc(1, sizeof(*job));
    httpd_req_t *async_req = NULL;
    if (job != NULL && httpd_req_async_handler_begin(req, &async_req) == ESP_OK)
    {
        job->req = async_req;
        job->st = st;
        snprintf(job->cliente, sizeof(job->cliente), "%s", cliente);
        snprintf(job->estado, sizeof(job->estado), "%s", estado);
        snprintf(job->caminho, sizeof(job->caminho), "%s", caminho);
        if (xQueueSend(s_job_queue, &job, 0) == pdTRUE)
        {
            return ESP_OK; /* worker replies and completes the request */
        }
        httpd_req_async_handler_complete(async_req);
    }

    free(job);
    st.status = ST_ERROR;
    add_msg(&st, "could not schedule the run");
    state_commit(&st);
    ESP_LOGE(TAG, "could not schedule run %.0f", st.id_num);
    return send_status(req, "500 Internal Server Error", &st);
}

/* Worker side: reply to the (async) request, release it and free the job. */
static void job_finish(job_t *job, const char *http_status, const run_state_t *st)
{
    send_status(job->req, http_status, st);
    httpd_req_async_handler_complete(job->req);
    free(job);
}

static void run_job(job_t *job)
{
    run_state_t st = job->st;
    const char *cliente = job->cliente;
    const char *estado = job->estado;
    const char *caminho = job->caminho;

    /* --- Resolve preset: cliente, estado, default --- */
    char urls[3][PRESET_URL_LEN];
    int url_count = 0;
    if (cliente[0] != '\0')
    {
        snprintf(urls[url_count++], PRESET_URL_LEN, "%s%s/%s", caminho, cliente, PRESET_FILE_NAME);
    }
    if (estado[0] != '\0')
    {
        snprintf(urls[url_count++], PRESET_URL_LEN, "%s%s/%s", caminho, estado, PRESET_FILE_NAME);
    }
    snprintf(urls[url_count++], PRESET_URL_LEN, "%s%s", caminho, PRESET_FILE_NAME);

    fetch_ctx_t ctx = {.cap = PRESET_MAX_HEX_CHARS};
    ctx.buf = malloc(PRESET_MAX_HEX_CHARS + 1);
    char *clean = malloc(PRESET_MAX_HEX_CHARS + 1);
    uint8_t *bin = malloc(PRESET_MAX_HEX_CHARS / 2);
    if (ctx.buf == NULL || clean == NULL || bin == NULL)
    {
        free(ctx.buf);
        free(clean);
        free(bin);
        st.status = ST_ERROR;
        add_msg(&st, "out of memory");
        state_commit(&st);
        ESP_LOGE(TAG, "out of memory for preset buffers");
        job_finish(job, "500 Internal Server Error", &st);
        return;
    }

    bool found = false;
    bool only_not_found = true;
    for (int i = 0; i < url_count && !found; i++)
    {
        esp_err_t terr = ESP_OK;
        int code = fetch_preset(urls[i], &ctx, &terr);
        if (code < 0)
        {
            only_not_found = false;
            add_msg(&st, "%s: %s", urls[i], esp_err_to_name(terr));
            ESP_LOGW(TAG, "preset %s -> %s", urls[i], esp_err_to_name(terr));
        }
        else if (code == 404)
        {
            add_msg(&st, "%s: not found (404)", urls[i]);
            ESP_LOGI(TAG, "preset %s -> 404, trying next", urls[i]);
        }
        else if (code == 200 && !ctx.overflow)
        {
            ESP_LOGI(TAG, "preset found: %s (%u chars)", urls[i], (unsigned)ctx.len);
            found = true;
        }
        else
        {
            only_not_found = false;
            if (code == 200)
            {
                add_msg(&st, "%s: file too large (max %d hex chars)", urls[i], PRESET_MAX_HEX_CHARS);
            }
            else
            {
                add_msg(&st, "%s: HTTP %d", urls[i], code);
            }
            ESP_LOGW(TAG, "preset %s -> HTTP %d%s", urls[i], code, ctx.overflow ? " (too large)" : "");
        }
    }

    if (!found)
    {
        free(ctx.buf);
        free(clean);
        free(bin);
        st.status = ST_ERROR;
        state_commit(&st);
        if (only_not_found)
        {
            ESP_LOGW(TAG, "run %.0f failed: preset not found", st.id_num);
            job_finish(job, "404 Not Found", &st);
            return;
        }
        ESP_LOGW(TAG, "run %.0f failed: preset download error", st.id_num);
        job_finish(job, "500 Internal Server Error", &st);
        return;
    }

    /* --- Decode hex --- */
    int nbytes = hex_decode(ctx.buf, ctx.len, bin, PRESET_MAX_HEX_CHARS / 2, clean);
    if (nbytes < 0)
    {
        free(ctx.buf);
        free(clean);
        free(bin);
        st.msg_count = 0;
        st.status = ST_ERROR;
        add_msg(&st, "preset file is not valid hex");
        state_commit(&st);
        ESP_LOGW(TAG, "run %.0f failed: invalid hex content", st.id_num);
        job_finish(job, "500 Internal Server Error", &st);
        return;
    }

    /* --- "Action": log only (UART protocol will come later) --- */
    ESP_LOGI(TAG, "%s", PRESET_SIM_ACTION_TEXT);
    ESP_LOGI(TAG, "preset (%d bytes): %s", nbytes, clean);
    free(ctx.buf);
    free(clean);
    free(bin);

    /* Emulate UART transmit time so the Stinger timeout is exercised realistically */
    uint32_t tx_ms = ((uint32_t)nbytes * 10u * 1000u) / PRESET_UART_BAUD + 1u;
    vTaskDelay(pdMS_TO_TICKS(tx_ms));

    st.msg_count = 0; /* success: drop the "not found" notes of skipped locations */
    st.status = ST_RUNNING;
    state_commit(&st);
    esp_timer_stop(s_sim_timer); /* ignore error if not running */
    esp_timer_start_once(s_sim_timer, (uint64_t)PRESET_SIM_RUN_MS * 1000ULL);
    ESP_LOGI(TAG, "run %.0f started (simulated, finishes in %d ms)", st.id_num, PRESET_SIM_RUN_MS);

    job_finish(job, "200 OK", &st);
}

static void worker_task(void *arg)
{
    job_t *job = NULL;
    while (1)
    {
        if (xQueueReceive(s_job_queue, &job, portMAX_DELAY) == pdTRUE && job != NULL)
        {
            run_job(job);
        }
    }
}

/* ─── GET /status ────────────────────────────────────────────────────────── */
static esp_err_t status_get_handler(httpd_req_t *req)
{
    if (!auth_ok(req))
    {
        return send_unauthorized(req);
    }
    run_state_t st;
    state_snapshot(&st);
    return send_status(req, "200 OK", &st);
}

/* ─── GET / and /index.html : unauthenticated device info page ───────────── */
#define INFO_PAGE_CAP 3072

typedef struct
{
    char *buf;
    size_t len;
} sb_t;

static void sb_add(sb_t *s, const char *fmt, ...)
{
    if (s->len >= INFO_PAGE_CAP - 1)
    {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(s->buf + s->len, INFO_PAGE_CAP - s->len, fmt, ap);
    va_end(ap);
    if (n > 0)
    {
        s->len += ((size_t)n < INFO_PAGE_CAP - s->len) ? (size_t)n : (INFO_PAGE_CAP - s->len - 1);
    }
}

/* Append text with HTML escaping (SSID and other external strings) */
static void sb_add_esc(sb_t *s, const char *txt)
{
    for (; *txt; txt++)
    {
        switch (*txt)
        {
        case '<': sb_add(s, "&lt;"); break;
        case '>': sb_add(s, "&gt;"); break;
        case '&': sb_add(s, "&amp;"); break;
        case '"': sb_add(s, "&quot;"); break;
        default:
            sb_add(s, "%c", ((unsigned char)*txt < 0x20 || (unsigned char)*txt > 0x7E) ? '?' : *txt);
        }
    }
}

static void info_row(sb_t *s, const char *label, const char *fmt, ...)
{
    char val[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(val, sizeof(val), fmt, ap);
    va_end(ap);
    sb_add(s, "<tr><th>%s</th><td>", label);
    sb_add_esc(s, val);
    sb_add(s, "</td></tr>");
}

static void info_iface(sb_t *s, const char *name, esp_netif_t *netif)
{
    char label[40];
    uint8_t mac[6];
    esp_netif_ip_info_t ip;
    if (netif == NULL)
    {
        snprintf(label, sizeof(label), "%s", name);
        info_row(s, label, "not available");
        return;
    }
    if (esp_netif_get_mac(netif, mac) == ESP_OK)
    {
        snprintf(label, sizeof(label), "%s MAC", name);
        info_row(s, label, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    snprintf(label, sizeof(label), "%s IP", name);
    if (esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0)
    {
        info_row(s, label, IPSTR, IP2STR(&ip.ip));
    }
    else
    {
        info_row(s, label, "no IP");
    }
}

static const char *reset_reason_str(esp_reset_reason_t r)
{
    switch (r)
    {
    case ESP_RST_POWERON: return "power on";
    case ESP_RST_SW: return "software reset";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "other watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep wake";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_EXT: return "external pin";
    default: return "unknown";
    }
}

static esp_err_t index_get_handler(httpd_req_t *req)
{
    char *page = malloc(INFO_PAGE_CAP);
    if (page == NULL)
    {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    }
    sb_t s = {.buf = page, .len = 0};
    page[0] = '\0';

    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *part = esp_ota_get_running_partition();
    int64_t up = esp_timer_get_time() / 1000000LL;
    run_state_t st;
    state_snapshot(&st);

    sb_add(&s,
           "<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"utf-8\">"
           "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
           "<title>Prog Wi-Fi</title>"
           "<style>body{font-family:sans-serif;margin:1.5em}table{border-collapse:collapse}"
           "th,td{text-align:left;padding:.3em .8em;border-bottom:1px solid #ddd}"
           "th{color:#555;font-weight:600}</style></head><body><h1>Prog Wi-Fi</h1><table>");

    info_row(&s, "Project", "%s", app->project_name);
    info_row(&s, "Firmware version", "%s", app->version);
    info_row(&s, "Build", "%s %s", app->date, app->time);
    info_row(&s, "ESP-IDF", "%s", app->idf_ver);
    info_row(&s, "Hardware version", "%d", HW_VERSION);
    info_row(&s, "Running partition", "%s", part != NULL ? part->label : "?");

    if (!wifi_is_enabled())
    {
        info_row(&s, "Wi-Fi", "DISABLED (WIFI_ENABLED = 0 in firmware)");
    }
    else
    {
        info_iface(&s, "Wi-Fi", wifi_get_netif());
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
        {
            info_row(&s, "Wi-Fi SSID", "%s (%d dBm)", (const char *)ap.ssid, ap.rssi);
        }
        else
        {
            info_row(&s, "Wi-Fi SSID", "not connected");
        }
    }
    info_iface(&s, "Ethernet", ethernet_get_netif());

    info_row(&s, "Uptime", "%lldd %02lld:%02lld:%02lld", up / 86400, (up / 3600) % 24, (up / 60) % 60, up % 60);
    info_row(&s, "Free heap", "%u bytes (min %u)", (unsigned)esp_get_free_heap_size(),
             (unsigned)esp_get_minimum_free_heap_size());
    info_row(&s, "Reset reason", "%s", reset_reason_str(esp_reset_reason()));

    static const char *const run_txt[] = {"idle", "running", "finished OK", "finished with error"};
    info_row(&s, "Test run", "%s", run_txt[st.status & 3]);
    if (st.status != ST_IDLE)
    {
        if (st.id_is_string)
        {
            info_row(&s, "Test run id", "%s", st.id_str);
        }
        else
        {
            info_row(&s, "Test run id", "%.0f", st.id_num);
        }
    }

    sb_add(&s, "</table></body></html>");

    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, page, (ssize_t)s.len);
    free(page);
    return err;
}

/* ─── GET|POST /console : open the log console port (basic auth) ─────────── */
static bool basic_auth_ok(httpd_req_t *req)
{
    char hdr[160];
    size_t len = httpd_req_get_hdr_value_len(req, "Authorization");
    if (len == 0 || len >= sizeof(hdr))
    {
        return false;
    }
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) != ESP_OK ||
        strncasecmp(hdr, "Basic ", 6) != 0)
    {
        return false;
    }
    unsigned char decoded[96];
    size_t dlen = 0;
    const char *b64 = hdr + 6;
    while (*b64 == ' ')
    {
        b64++;
    }
    if (mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &dlen, (const unsigned char *)b64, strlen(b64)) != 0)
    {
        return false;
    }
    decoded[dlen] = '\0';

    /* Constant-time compare against "user:pass" */
    const char *expected = CONSOLE_AUTH_USER ":" CONSOLE_AUTH_PASS;
    size_t elen = strlen(expected);
    if (dlen != elen)
    {
        return false;
    }
    unsigned char diff = 0;
    for (size_t i = 0; i < elen; i++)
    {
        diff |= (unsigned char)(decoded[i] ^ (unsigned char)expected[i]);
    }
    return diff == 0;
}

static esp_err_t console_handler(httpd_req_t *req)
{
    if (!basic_auth_ok(req))
    {
        ESP_LOGW(TAG, "%s /console rejected (401): bad or missing basic auth", req->method == HTTP_POST ? "POST" : "GET");
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"Prog\"");
        return httpd_resp_send(req, NULL, 0);
    }

    char msg[128];
    const char *http = "200 OK";
    switch (log_server_request_open())
    {
    case LOG_OPEN_STARTED:
        snprintf(msg, sizeof(msg), "Log console open on TCP port %d for %d s. Connect now.\n",
                 LOG_SERVER_PORT, LOG_SERVER_WINDOW_S);
        break;
    case LOG_OPEN_EXTENDED:
        snprintf(msg, sizeof(msg), "Log console already open on TCP port %d. Window restarted: %d s.\n",
                 LOG_SERVER_PORT, LOG_SERVER_WINDOW_S);
        break;
    case LOG_OPEN_BUSY:
        http = "409 Conflict";
        snprintf(msg, sizeof(msg), "Log console in use by another client.\n");
        break;
    default:
        http = "500 Internal Server Error";
        snprintf(msg, sizeof(msg), "Log console not available.\n");
        break;
    }
    ESP_LOGI(TAG, "/console authenticated: %s", http);
    httpd_resp_set_status(req, http);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, msg, HTTPD_RESP_USE_STRLEN);
}

/* ─── Start ──────────────────────────────────────────────────────────────── */
void preset_server_start(void)
{
    if (s_server != NULL)
    {
        return;
    }

    s_lock = xSemaphoreCreateMutex();
    s_job_queue = xQueueCreate(1, sizeof(job_t *));
    const esp_timer_create_args_t targs = {
        .callback = sim_run_done_cb,
        .name = "sim_run",
    };
    if (s_lock == NULL || s_job_queue == NULL || esp_timer_create(&targs, &s_sim_timer) != ESP_OK ||
        xTaskCreate(worker_task, "preset_worker", 12288, NULL, 5, NULL) != pdPASS) /* runs the TLS client */
    {
        ESP_LOGE(TAG, "init failed (mutex/queue/timer/worker)");
        return;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = PRESET_SERVER_PORT;
    config.stack_size = 6144;
    config.max_open_sockets = 4;
    config.lru_purge_enable = true;
    config.max_uri_handlers = 8;
    config.recv_wait_timeout = 10;

    if (httpd_start(&s_server, &config) != ESP_OK)
    {
        ESP_LOGE(TAG, "httpd_start failed");
        s_server = NULL;
        return;
    }

    const httpd_uri_t post_uri = {
        .uri = "/iniciar",
        .method = HTTP_POST,
        .handler = iniciar_post_handler,
    };
    const httpd_uri_t status_uri = {
        .uri = "/status",
        .method = HTTP_GET,
        .handler = status_get_handler,
    };
    const httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = index_get_handler,
    };
    const httpd_uri_t index_uri = {
        .uri = "/index.html",
        .method = HTTP_GET,
        .handler = index_get_handler,
    };
    const httpd_uri_t console_get_uri = {
        .uri = "/console",
        .method = HTTP_GET,
        .handler = console_handler,
    };
    const httpd_uri_t console_post_uri = {
        .uri = "/console",
        .method = HTTP_POST,
        .handler = console_handler,
    };
    httpd_register_uri_handler(s_server, &console_get_uri);
    httpd_register_uri_handler(s_server, &console_post_uri);
    httpd_register_uri_handler(s_server, &post_uri);
    httpd_register_uri_handler(s_server, &status_uri);
    httpd_register_uri_handler(s_server, &root_uri);
    httpd_register_uri_handler(s_server, &index_uri);
    ESP_LOGI(TAG, "HTTP API listening on port %d (POST /iniciar, GET /status)", PRESET_SERVER_PORT);
}
