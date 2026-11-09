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
#include "esp_image_format.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/idf_additions.h"
#include "device_info.h"
#include "ethernet_manager.h"
#include "wifi_manager.h"
#include "mbedtls/base64.h"
#include "log_server.h"
#include "nvs_schema.h"
#include "preset_server.h"
#include "admin_api.h"
#include "http_log.h"

#define TAG "PRESET"

/* ─── Inline constants (phase 1) ─────────────────────────────────────────── */
#define PRESET_API_KEY "abcdef12345XYZ"        /* expected X-API-Key value */
#define CONSOLE_AUTH_USER "admin"                  /* basic auth for /console */
#define CONSOLE_AUTH_PASS "Ppastinger123"
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

/* Decoded preset (binary) kept in static RAM: the last one that was downloaded AND decoded
 * successfully. A failed decode leaves it untouched. The worker task is the only writer;
 * the HTTP task reads it through preset_server_preset_snapshot(). Both sides hold
 * s_data_lock, which also protects the run history below. */
static uint8_t s_preset_bin[PRESET_BIN_MAX];
static size_t s_preset_len = 0;
static bool s_preset_valid = false;
static preset_info_t s_preset_info;
static SemaphoreHandle_t s_data_lock;

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

/* ─── Small data helpers ─────────────────────────────────────────────────── */

/* ASCII copy: anything outside 0x20..0x7E becomes '?' (keeps JSON output valid). */
static void copy_ascii(char *dst, size_t dst_len, const char *src)
{
    size_t n = 0;
    for (; src != NULL && src[n] != '\0' && n + 1 < dst_len; n++)
    {
        unsigned char c = (unsigned char)src[n];
        dst[n] = (c >= 0x20 && c <= 0x7E) ? (char)c : '?';
    }
    dst[n] = '\0';
}

static void format_id(const run_state_t *st, char *out, size_t out_len)
{
    if (st == NULL)
    {
        out[0] = '\0';
    }
    else if (st->id_is_string)
    {
        copy_ascii(out, out_len, st->id_str);
    }
    else
    {
        snprintf(out, out_len, "%.0f", st->id_num);
    }
}

/* Standard CRC-32 (IEEE 802.3, same as zlib / Python binascii.crc32). */
static uint32_t crc32_calc(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
    {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
        {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

/* ─── Run history (last PRESET_HISTORY_LEN requests to POST /iniciar) ───────
 * Slot = seq % PRESET_HISTORY_LEN, so an entry is found by its sequence number and
 * the oldest one is overwritten first. An entry is created when the request arrives
 * (result "running") and completed when the reply is decided. All access holds
 * s_data_lock. */
typedef struct
{
    preset_history_entry_t e;
    int64_t start_us;
} hist_slot_t;

static hist_slot_t s_hist[PRESET_HISTORY_LEN];
static uint32_t s_hist_next_seq = 1;
/* Sequence number of the /iniciar request being handled. esp_http_server runs handlers one
 * at a time in its own task, so a plain static is safe for the duration of the handler. */
static uint32_t s_req_seq;

static uint32_t hist_begin(void)
{
    if (s_data_lock == NULL)
    {
        return 0;
    }
    xSemaphoreTake(s_data_lock, portMAX_DELAY);
    uint32_t seq = s_hist_next_seq++;
    hist_slot_t *s = &s_hist[seq % PRESET_HISTORY_LEN];
    memset(s, 0, sizeof(*s));
    s->e.seq = seq;
    s->start_us = esp_timer_get_time();
    s->e.start_s = (uint32_t)(s->start_us / 1000000LL);
    s->e.result = PRESET_HRES_RUNNING;
    xSemaphoreGive(s_data_lock);
    return seq;
}

/* Caller holds s_data_lock. NULL if the slot was already reused. */
static hist_slot_t *hist_slot(uint32_t seq)
{
    if (seq == 0)
    {
        return NULL;
    }
    hist_slot_t *s = &s_hist[seq % PRESET_HISTORY_LEN];
    return (s->e.seq == seq) ? s : NULL;
}

/* Record what is known about the request. NULL arguments leave that field alone. */
static void hist_set_request(uint32_t seq, const run_state_t *st, const char *produto, const char *cliente,
                             const char *estado)
{
    if (s_data_lock == NULL)
    {
        return;
    }
    xSemaphoreTake(s_data_lock, portMAX_DELAY);
    hist_slot_t *s = hist_slot(seq);
    if (s != NULL)
    {
        if (st != NULL)
        {
            format_id(st, s->e.id, sizeof(s->e.id));
        }
        if (produto != NULL)
        {
            copy_ascii(s->e.product, sizeof(s->e.product), produto);
        }
        if (cliente != NULL)
        {
            copy_ascii(s->e.cliente, sizeof(s->e.cliente), cliente);
        }
        if (estado != NULL)
        {
            copy_ascii(s->e.estado, sizeof(s->e.estado), estado);
        }
    }
    xSemaphoreGive(s_data_lock);
}

/* Complete the entry with the HTTP code that was (or will be) sent. */
static void hist_end(uint32_t seq, int code, preset_level_t level, uint16_t bytes, bool has_crc, uint32_t crc)
{
    if (s_data_lock == NULL)
    {
        return;
    }
    xSemaphoreTake(s_data_lock, portMAX_DELAY);
    hist_slot_t *s = hist_slot(seq);
    if (s != NULL)
    {
        s->e.code = (uint16_t)code;
        s->e.result = (code == 200) ? PRESET_HRES_OK
                      : (code == 400 || code == 401 || code == 409) ? PRESET_HRES_REJECTED
                                                                     : PRESET_HRES_ERROR;
        s->e.level = level;
        s->e.bytes = bytes;
        s->e.has_crc = has_crc;
        s->e.crc32 = crc;
        int64_t ms = (esp_timer_get_time() - s->start_us) / 1000;
        s->e.duration_ms = (ms < 0) ? 0 : (uint32_t)ms;
    }
    xSemaphoreGive(s_data_lock);
}

size_t preset_server_history_snapshot(preset_history_entry_t *out, size_t max)
{
    size_t n = 0;
    if (s_data_lock == NULL)
    {
        return 0;
    }
    xSemaphoreTake(s_data_lock, portMAX_DELAY);
    for (uint32_t seq = s_hist_next_seq - 1;
         seq >= 1 && (s_hist_next_seq - seq) <= PRESET_HISTORY_LEN && n < max; seq--)
    {
        hist_slot_t *s = hist_slot(seq);
        if (s != NULL)
        {
            out[n++] = s->e;
        }
    }
    xSemaphoreGive(s_data_lock);
    return n;
}

bool preset_server_preset_snapshot(preset_info_t *info, uint8_t *bin, size_t bin_cap, size_t *bin_len)
{
    bool ok = false;
    if (s_data_lock == NULL)
    {
        return false;
    }
    xSemaphoreTake(s_data_lock, portMAX_DELAY);
    if (s_preset_valid && s_preset_len <= bin_cap)
    {
        *info = s_preset_info;
        memcpy(bin, s_preset_bin, s_preset_len);
        *bin_len = s_preset_len;
        ok = true;
    }
    xSemaphoreGive(s_data_lock);
    return ok;
}

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
typedef enum
{
    BEGIN_OK,
    BEGIN_RUN_ACTIVE, /* a run is already in progress (*cur = that run) */
    BEGIN_ADMIN_BUSY, /* firmware update / format / reboot in progress (state untouched) */
} begin_result_t;

/* Lock order: s_lock (here) -> admin lock (inside admin_api_blocks_runs()).
 * admin_api.c never takes s_lock while holding its own lock, so this cannot deadlock. */
static begin_result_t state_try_begin(const run_state_t *in, run_state_t *cur)
{
    begin_result_t res;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_state.status == ST_RUNNING)
    {
        *cur = s_state;
        res = BEGIN_RUN_ACTIVE;
    }
    else if (admin_api_blocks_runs())
    {
        res = BEGIN_ADMIN_BUSY;
    }
    else
    {
        s_state = *in;
        res = BEGIN_OK;
    }
    xSemaphoreGive(s_lock);
    return res;
}

/* Work item handed from the HTTP task to the worker task */
typedef struct
{
    httpd_req_t *req; /* async copy of the request, completed by the worker */
    run_state_t st;
    char cliente[PRESET_FIELD_LEN];
    char estado[PRESET_FIELD_LEN];
    char caminho[PRESET_BASE_LEN];
    /* History bookkeeping: seq is set by the handler, the rest by run_job() */
    uint32_t seq;
    preset_level_t level;
    uint16_t bytes;
    bool has_crc;
    uint32_t crc;
    int64_t t0_us; /* when the request arrived, for the request log */
    int stack_hw;  /* httpd task stack, lowest free bytes seen while handling it */
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
        http_log_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
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
        http_log_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }

    http_log_set_status(req, http_status);
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
    hist_set_request(s_req_seq, id_src, NULL, NULL, NULL);
    hist_end(s_req_seq, 400, PRESET_LVL_NA, 0, false, 0);
    return send_status(req, "400 Bad Request", &st);
}

static esp_err_t send_unauthorized(httpd_req_t *req)
{
    ESP_LOGW(TAG, "%s %s rejected (401): missing or wrong X-API-Key",
             req->method == HTTP_POST ? "POST" : "GET", req->uri);
    http_log_set_status(req, "401 Unauthorized");
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

/* ─── POST /iniciar ────────────────────────────────────────────────────────
 * Works on heap memory. The httpd task has a small stack and this handler used to keep
 * about 3.9 KB of buffers on it (request body, three run states, the text fields). A log
 * call that formats a double on top of that overflowed it. Everything big now lives in
 * job_t (kept for the worker) and iniciar_tmp_t (freed before the handler returns). */
typedef struct
{
    char body[PRESET_MAX_BODY];
    char produto[PRESET_FIELD_LEN];
    run_state_t other; /* the run in progress (409), or the "update in progress" reply */
} iniciar_tmp_t;

/* Returns the handler result. *handed_off is set when job now belongs to the worker. */
static esp_err_t iniciar_process(httpd_req_t *req, job_t *job, iniciar_tmp_t *tmp, bool *handed_off)
{
    run_state_t *st = &job->st;

    /* --- Parse body --- */
    if (read_body(req, tmp->body, sizeof(tmp->body)) < 0)
    {
        return send_bad_request(req, NULL, "empty, unreadable or too large body");
    }
    cJSON *root = cJSON_Parse(tmp->body);
    if (root == NULL || !cJSON_IsObject(root))
    {
        cJSON_Delete(root);
        return send_bad_request(req, NULL, "invalid JSON");
    }

    /* id: number or numeric string (echoed back with the same type) */
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
    if (cJSON_IsNumber(id))
    {
        st->id_num = id->valuedouble;
    }
    else if (cJSON_IsString(id) && id->valuestring != NULL)
    {
        snprintf(st->id_str, sizeof(st->id_str), "%s", id->valuestring);
        char *t = trim(st->id_str);
        char *end = NULL;
        st->id_num = strtod(t, &end);
        if (*t == '\0' || end == NULL || *end != '\0')
        {
            cJSON_Delete(root);
            st->id_str[0] = '\0';
            return send_bad_request(req, st, "id must be a number or numeric string");
        }
        memmove(st->id_str, t, strlen(t) + 1);
        st->id_is_string = true;
    }
    else
    {
        cJSON_Delete(root);
        return send_bad_request(req, NULL, "missing id");
    }

    get_str_field(root, "produto", tmp->produto, sizeof(tmp->produto));
    get_str_field(root, "cliente", job->cliente, sizeof(job->cliente));
    get_str_field(root, "estado", job->estado, sizeof(job->estado));
    get_str_field(root, "caminho", job->caminho, sizeof(job->caminho));
    cJSON_Delete(root);
    hist_set_request(s_req_seq, st, tmp->produto, job->cliente, job->estado);

    if (job->caminho[0] == '\0')
    {
        return send_bad_request(req, st, "missing caminho");
    }
    if (strncasecmp(job->caminho, "http://", 7) != 0 && strncasecmp(job->caminho, "https://", 8) != 0)
    {
        return send_bad_request(req, st, "caminho must start with http:// or https://");
    }
    if (segment_unsafe(job->cliente) || segment_unsafe(job->estado))
    {
        return send_bad_request(req, st, "cliente/estado contain invalid characters");
    }
    size_t clen = strlen(job->caminho);
    if (job->caminho[clen - 1] != '/')
    {
        if (clen + 1 >= sizeof(job->caminho))
        {
            return send_bad_request(req, st, "caminho too long");
        }
        job->caminho[clen] = '/';
        job->caminho[clen + 1] = '\0';
    }

    /* --- Busy check + accept (atomic): the run is "running" until the preset is handled --- */
    st->status = ST_RUNNING;
    switch (state_try_begin(st, &tmp->other))
    {
    case BEGIN_OK:
        break;
    case BEGIN_ADMIN_BUSY:
        /* Not a run conflict: reply with the request id and status 3 (the stored state is untouched) */
        tmp->other = *st;
        tmp->other.status = ST_ERROR;
        tmp->other.msg_count = 0;
        add_msg(&tmp->other, "firmware update in progress, try again later");
        ESP_LOGW(TAG, "POST /iniciar rejected (409): firmware update / restart in progress");
        hist_end(s_req_seq, 409, PRESET_LVL_NA, 0, false, 0);
        return send_status(req, "409 Conflict", &tmp->other);
    default: /* BEGIN_RUN_ACTIVE: tmp->other holds the run in progress */
        ESP_LOGW(TAG, "POST /iniciar rejected (409): a run is already in progress");
        hist_end(s_req_seq, 409, PRESET_LVL_NA, 0, false, 0);
        return send_status(req, "409 Conflict", &tmp->other);
    }

    char idtxt[24];
    format_id(st, idtxt, sizeof(idtxt)); /* text, so the log call itself never formats a double */
    ESP_LOGI(TAG, "POST /iniciar id=%s produto='%s' cliente='%s' estado='%s' caminho='%s'",
             idtxt, tmp->produto, job->cliente, job->estado, job->caminho);

    /* --- Hand the request to the worker so the HTTP task stays free (/status, 409) --- */
    httpd_req_t *async_req = NULL;
    if (httpd_req_async_handler_begin(req, &async_req) == ESP_OK)
    {
        job->req = async_req;
        job->seq = s_req_seq;
        job->t0_us = http_log_request_start_us();
        job->stack_hw = http_log_stack_free_min();
        if (xQueueSend(s_job_queue, &job, 0) == pdTRUE)
        {
            http_log_async_detach(); /* the worker logs this request when it replies */
            *handed_off = true;
            return ESP_OK; /* worker replies and completes the request */
        }
        httpd_req_async_handler_complete(async_req);
    }

    st->status = ST_ERROR;
    add_msg(st, "could not schedule the run");
    state_commit(st);
    ESP_LOGE(TAG, "could not schedule run %s", idtxt);
    hist_end(s_req_seq, 500, PRESET_LVL_NA, 0, false, 0);
    return send_status(req, "500 Internal Server Error", st);
}

static esp_err_t iniciar_post_handler(httpd_req_t *req)
{
    s_req_seq = hist_begin();
    if (!auth_ok(req))
    {
        hist_end(s_req_seq, 401, PRESET_LVL_NA, 0, false, 0);
        return send_unauthorized(req);
    }

    job_t *job = calloc(1, sizeof(*job));
    iniciar_tmp_t *tmp = malloc(sizeof(*tmp));
    if (job == NULL || tmp == NULL)
    {
        free(job);
        free(tmp);
        ESP_LOGE(TAG, "out of memory for POST /iniciar");
        hist_end(s_req_seq, 500, PRESET_LVL_NA, 0, false, 0);
        http_log_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"id\":0,\"status\":3,\"message\":[\"out of memory\"]}", HTTPD_RESP_USE_STRLEN);
    }

    bool handed_off = false;
    esp_err_t err = iniciar_process(req, job, tmp, &handed_off);
    free(tmp);
    if (!handed_off)
    {
        free(job);
    }
    return err;
}
/* Worker side: reply to the (async) request, release it and free the job. */
static void job_finish(job_t *job, const char *http_status, const run_state_t *st)
{
    hist_end(job->seq, atoi(http_status), job->level, job->bytes, job->has_crc, job->crc);
    send_status(job->req, http_status, st);
    http_log_async_done(job->req, http_status, job->t0_us, job->stack_hw);
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
    preset_level_t lvls[3]; /* which lookup level each URL is */
    int found_idx = -1;
    int url_count = 0;
    if (cliente[0] != '\0')
    {
        lvls[url_count] = PRESET_LVL_CUSTOMER;
        snprintf(urls[url_count++], PRESET_URL_LEN, "%s%s/%s", caminho, cliente, PRESET_FILE_NAME);
    }
    if (estado[0] != '\0')
    {
        lvls[url_count] = PRESET_LVL_STATE;
        snprintf(urls[url_count++], PRESET_URL_LEN, "%s%s/%s", caminho, estado, PRESET_FILE_NAME);
    }
    lvls[url_count] = PRESET_LVL_DEFAULT;
    snprintf(urls[url_count++], PRESET_URL_LEN, "%s%s", caminho, PRESET_FILE_NAME);

    fetch_ctx_t ctx = {.cap = PRESET_MAX_HEX_CHARS};
    ctx.buf = malloc(PRESET_MAX_HEX_CHARS + 1);
    char *clean = malloc(PRESET_MAX_HEX_CHARS + 1);
    if (ctx.buf == NULL || clean == NULL)
    {
        free(ctx.buf);
        free(clean);
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
            found_idx = i;
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
        st.status = ST_ERROR;
        state_commit(&st);
        job->level = only_not_found ? PRESET_LVL_NONE : PRESET_LVL_NA;
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

    /* --- Decode hex straight into the static buffer. hex_decode() validates everything
     *     before it writes a byte, so a failure leaves the previous preset untouched. --- */
    job->level = lvls[found_idx];
    xSemaphoreTake(s_data_lock, portMAX_DELAY);
    int nbytes = hex_decode(ctx.buf, ctx.len, s_preset_bin, PRESET_BIN_MAX, clean);
    if (nbytes >= 0)
    {
        s_preset_len = (size_t)nbytes;
        s_preset_valid = true;
        s_preset_info.seq = job->seq;
        format_id(&st, s_preset_info.id, sizeof(s_preset_info.id));
        copy_ascii(s_preset_info.source, sizeof(s_preset_info.source), urls[found_idx]);
        s_preset_info.level = job->level;
        s_preset_info.bytes = (uint16_t)nbytes;
        s_preset_info.crc32 = crc32_calc(s_preset_bin, (size_t)nbytes);
        s_preset_info.loaded_s = (uint32_t)(esp_timer_get_time() / 1000000LL);
        job->bytes = (uint16_t)nbytes;
        job->crc = s_preset_info.crc32;
        job->has_crc = true;
    }
    xSemaphoreGive(s_data_lock);
    if (nbytes < 0)
    {
        free(ctx.buf);
        free(clean);
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

/* ─── Static assets (embedded in flash) ───────────────────────────────────
 * To move assets to SPIFFS/LittleFS later, only serve_asset() callers change. */
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");

/* EMBED_TXTFILES appends a NUL byte, which is not part of the content. */
static esp_err_t serve_asset(httpd_req_t *req, const char *type, const uint8_t *start, const uint8_t *end,
                             const char *cache_control)
{
    httpd_resp_set_type(req, type);
    httpd_resp_set_hdr(req, "Cache-Control", cache_control);
    return httpd_resp_send(req, (const char *)start, (ssize_t)(end - start - 1));
}

/* ─── GET / and /index.html : static, unauthenticated page ───────────────── */
static esp_err_t index_get_handler(httpd_req_t *req)
{
    return serve_asset(req, "text/html", index_html_start, index_html_end, "no-cache");
}

/* ─── CPU usage sampler ───────────────────────────────────────────────────
 * Every CPU_SAMPLE_MS an esp_timer callback compares the idle tasks' run time
 * counters (esp_timer clock, microseconds) with the elapsed wall time:
 *   usage = 1 - idle_time / (elapsed * number_of_cores)
 * /info reports the result of the last window (all cores combined). */
#define CPU_SAMPLE_MS 2000

static esp_timer_handle_t s_cpu_timer;
static volatile int s_cpu_permille = -1; /* tenths of a percent, -1 until the first window is done */

static void cpu_sample_cb(void *arg)
{
    static uint32_t prev_idle[configNUMBER_OF_CORES];
    static int64_t prev_wall = 0;
    static bool primed = false;

    int64_t now = esp_timer_get_time();
    uint64_t idle_delta = 0;
    for (int core = 0; core < configNUMBER_OF_CORES; core++)
    {
        uint32_t cur = (uint32_t)ulTaskGetIdleRunTimeCounterForCore(core);
        idle_delta += (uint32_t)(cur - prev_idle[core]); /* modular: survives counter wrap */
        prev_idle[core] = cur;
    }
    if (primed && now > prev_wall)
    {
        uint64_t capacity = (uint64_t)(now - prev_wall) * configNUMBER_OF_CORES;
        int idle_pm = (int)((idle_delta * 1000ULL) / capacity);
        if (idle_pm > 1000)
        {
            idle_pm = 1000;
        }
        s_cpu_permille = 1000 - idle_pm;
    }
    prev_wall = now;
    primed = true;
}

/* ─── GET /info : unauthenticated device info (JSON) ─────────────────────── */
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

static const char *ota_state_str(esp_ota_img_states_t s)
{
    switch (s)
    {
    case ESP_OTA_IMG_NEW: return "new";
    case ESP_OTA_IMG_PENDING_VERIFY: return "pending_verify";
    case ESP_OTA_IMG_VALID: return "valid";
    case ESP_OTA_IMG_INVALID: return "invalid";
    case ESP_OTA_IMG_ABORTED: return "aborted";
    case ESP_OTA_IMG_UNDEFINED: return "undefined";
    default: return NULL;
    }
}

/* Add {"mac": "...", "ip": "..."|null} (or null when the interface does not exist) */
static void json_add_iface(cJSON *parent, const char *key, esp_netif_t *netif)
{
    if (netif == NULL)
    {
        cJSON_AddNullToObject(parent, key);
        return;
    }
    cJSON *o = cJSON_AddObjectToObject(parent, key);
    if (o == NULL)
    {
        return;
    }
    uint8_t mac[6];
    char buf[24];
    if (esp_netif_get_mac(netif, mac) == ESP_OK)
    {
        snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        cJSON_AddStringToObject(o, "mac", buf);
    }
    else
    {
        cJSON_AddNullToObject(o, "mac");
    }
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0)
    {
        snprintf(buf, sizeof(buf), IPSTR, IP2STR(&ip.ip));
        cJSON_AddStringToObject(o, "ip", buf);
    }
    else
    {
        cJSON_AddNullToObject(o, "ip");
    }
}

static esp_err_t info_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL)
    {
        return http_log_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    }

    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *part = esp_ota_get_running_partition();
    char buf[64];

    cJSON_AddStringToObject(root, "project", app->project_name);
    cJSON_AddStringToObject(root, "version", app->version);
    snprintf(buf, sizeof(buf), "%s %s", app->date, app->time);
    cJSON_AddStringToObject(root, "build", buf);
    cJSON_AddStringToObject(root, "idf", app->idf_ver);
    cJSON_AddNumberToObject(root, "hw_version", HW_VERSION);
    cJSON_AddStringToObject(root, "partition", part != NULL ? part->label : "?");

    /* Wi-Fi */
    cJSON *wifi = cJSON_AddObjectToObject(root, "wifi");
    if (wifi != NULL)
    {
        cJSON_AddBoolToObject(wifi, "enabled", wifi_is_enabled());
        if (wifi_is_enabled())
        {
            json_add_iface(wifi, "iface", wifi_get_netif());
            wifi_ap_record_t ap;
            if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
            {
                char ssid[sizeof(ap.ssid) + 1];
                size_t k = 0;
                for (; k < sizeof(ap.ssid) && ap.ssid[k]; k++) /* ASCII only, valid JSON/UTF-8 */
                {
                    ssid[k] = (ap.ssid[k] >= 0x20 && ap.ssid[k] <= 0x7E) ? (char)ap.ssid[k] : '?';
                }
                ssid[k] = '\0';
                cJSON_AddStringToObject(wifi, "ssid", ssid);
                cJSON_AddNumberToObject(wifi, "rssi", ap.rssi);
            }
            else
            {
                cJSON_AddNullToObject(wifi, "ssid");
            }
        }
    }
    json_add_iface(root, "ethernet", ethernet_get_netif());
    cJSON *eth = cJSON_GetObjectItemCaseSensitive(root, "ethernet");
    if (cJSON_IsObject(eth))
    {
        int speed = 0;
        bool full = false;
        bool up = ethernet_get_link(&speed, &full);
        cJSON_AddBoolToObject(eth, "link_up", up);
        if (up)
        {
            cJSON_AddNumberToObject(eth, "speed_mbps", speed);
            cJSON_AddStringToObject(eth, "duplex", full ? "full" : "half");
        }
        else
        {
            cJSON_AddNullToObject(eth, "speed_mbps");
            cJSON_AddNullToObject(eth, "duplex");
        }
    }

    cJSON_AddNumberToObject(root, "uptime_s", (double)(esp_timer_get_time() / 1000000LL));

    int cpu_pm = s_cpu_permille;
    if (cpu_pm >= 0)
    {
        cJSON_AddNumberToObject(root, "cpu_percent", cpu_pm / 10.0);
    }
    else
    {
        cJSON_AddNullToObject(root, "cpu_percent"); /* first sample window not finished yet */
    }

    /* App partition usage: used image length vs. partition size */
    if (part != NULL)
    {
        cJSON *pu = cJSON_AddObjectToObject(root, "partition_usage");
        if (pu != NULL)
        {
            esp_image_metadata_t meta = {0};
            const esp_partition_pos_t pos = {.offset = part->address, .size = part->size};
            cJSON_AddNumberToObject(pu, "total_kb", part->size / 1024);
            if (esp_image_get_metadata(&pos, &meta) == ESP_OK)
            {
                cJSON_AddNumberToObject(pu, "used_kb", meta.image_len / 1024);
                cJSON_AddNumberToObject(pu, "percent", 100.0 * (double)meta.image_len / (double)part->size);
            }
        }
    }

    cJSON *heap = cJSON_AddObjectToObject(root, "heap");
    if (heap != NULL)
    {
        cJSON_AddNumberToObject(heap, "free", esp_get_free_heap_size());
        cJSON_AddNumberToObject(heap, "min_free", esp_get_minimum_free_heap_size());
        /* DMA-capable heap: the ENC28J60 receive buffers come from here */
        cJSON_AddNumberToObject(heap, "dma_free", heap_caps_get_free_size(MALLOC_CAP_DMA));
        cJSON_AddNumberToObject(heap, "dma_min_free", heap_caps_get_minimum_free_size(MALLOC_CAP_DMA));
        cJSON_AddNumberToObject(heap, "dma_largest", heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
    }
    cJSON_AddStringToObject(root, "reset_reason", reset_reason_str(esp_reset_reason()));

    uint32_t boot_cnt = 0, panic_tot = 0;
    if (nvs_boot_counters_get(&boot_cnt, &panic_tot))
    {
        cJSON_AddNumberToObject(root, "boot_count", boot_cnt);
        cJSON_AddNumberToObject(root, "panic_count", panic_tot);
    }
    else
    {
        cJSON_AddNullToObject(root, "boot_count");
        cJSON_AddNullToObject(root, "panic_count");
    }

    esp_ota_img_states_t ota_state;
    const char *ota_txt = (part != NULL && esp_ota_get_state_partition(part, &ota_state) == ESP_OK)
                              ? ota_state_str(ota_state)
                              : NULL;
    if (ota_txt != NULL)
    {
        cJSON_AddStringToObject(root, "ota_state", ota_txt);
    }
    else
    {
        cJSON_AddNullToObject(root, "ota_state"); /* e.g. flashed over serial: no OTA state recorded */
    }

    /* Test run */
    static const char *const run_txt[] = {"idle", "running", "finished OK", "finished with error"};
    run_state_t st;
    state_snapshot(&st);
    cJSON *run = cJSON_AddObjectToObject(root, "test_run");
    if (run != NULL)
    {
        cJSON_AddNumberToObject(run, "status", st.status);
        cJSON_AddStringToObject(run, "status_text", run_txt[st.status & 3]);
        if (st.status == ST_IDLE)
        {
            cJSON_AddNullToObject(run, "id");
        }
        else if (st.id_is_string)
        {
            cJSON_AddStringToObject(run, "id", st.id_str);
        }
        else
        {
            cJSON_AddNumberToObject(run, "id", st.id_num);
        }
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL)
    {
        return http_log_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    free(json);
    return err;
}

/* ─── GET|POST /console : open the log console port (basic auth) ─────────── */
bool preset_server_basic_auth_ok(httpd_req_t *req)
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

bool preset_server_run_active(void)
{
    run_state_t st;
    state_snapshot(&st);
    return st.status == ST_RUNNING;
}

bool preset_server_running(void)
{
    return s_server != NULL;
}

static esp_err_t console_handler(httpd_req_t *req)
{
    if (!preset_server_basic_auth_ok(req))
    {
        ESP_LOGW(TAG, "%s /console rejected (401): bad or missing basic auth", req->method == HTTP_POST ? "POST" : "GET");
        http_log_set_status(req, "401 Unauthorized");
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
    http_log_set_status(req, http);
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
    s_data_lock = xSemaphoreCreateMutex();
    s_job_queue = xQueueCreate(1, sizeof(job_t *));
    const esp_timer_create_args_t targs = {
        .callback = sim_run_done_cb,
        .name = "sim_run",
    };
    if (s_lock == NULL || s_data_lock == NULL || s_job_queue == NULL || esp_timer_create(&targs, &s_sim_timer) != ESP_OK ||
        xTaskCreate(worker_task, "preset_worker", 12288, NULL, 5, NULL) != pdPASS) /* runs the TLS client */
    {
        ESP_LOGE(TAG, "init failed (mutex/queue/timer/worker)");
        return;
    }

    const esp_timer_create_args_t cpu_args = {
        .callback = cpu_sample_cb,
        .name = "cpu_sample",
    };
    if (esp_timer_create(&cpu_args, &s_cpu_timer) == ESP_OK)
    {
        esp_timer_start_periodic(s_cpu_timer, (uint64_t)CPU_SAMPLE_MS * 1000ULL);
    }
    else
    {
        ESP_LOGW(TAG, "CPU usage sampler not started");
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = PRESET_SERVER_PORT;
    config.stack_size = 8192; /* was 6144; see the stack note at POST /iniciar. The request log
                                 reports the lowest free stack seen, to keep an eye on it */
    config.max_open_sockets = 4;
    config.lru_purge_enable = true;
    config.max_uri_handlers = 16; /* 7 here + 5 admin endpoints (admin_api.c) */
    config.recv_wait_timeout = 10;

    if (httpd_start(&s_server, &config) != ESP_OK)
    {
        ESP_LOGE(TAG, "httpd_start failed");
        s_server = NULL;
        return;
    }
    http_log_install_error_handlers(s_server);

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
    http_log_register(s_server, &console_get_uri);
    http_log_register(s_server, &console_post_uri);
    const httpd_uri_t info_uri = {
        .uri = "/info",
        .method = HTTP_GET,
        .handler = info_get_handler,
    };
    http_log_register(s_server, &info_uri);
    http_log_register(s_server, &post_uri);
    http_log_register(s_server, &status_uri);
    http_log_register(s_server, &root_uri);
    http_log_register(s_server, &index_uri);
    admin_api_register(s_server);
    ESP_LOGI(TAG, "HTTP API listening on port %d (POST /iniciar, GET /status)", PRESET_SERVER_PORT);
}
