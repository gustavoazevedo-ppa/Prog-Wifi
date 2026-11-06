/****************************************************************************
 * Remote log console: ESP log output -> RAM ring buffer -> TCP client(s)
 *
 * The vprintf hook only formats the line and copies it into the ring (short
 * critical section, no allocation, no logging, no sockets), so a slow or
 * stalled client can never block the code that is logging. The listener task
 * reads from the ring and does all the socket I/O.
 ****************************************************************************/

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "log_server.h"

#define TAG "LOGSRV"

#define LOG_RING_SIZE 4096 /* must be a power of two */
#define LOG_LINE_MAX 320   /* longer lines are truncated with "..." */
#define LOG_CHUNK 200      /* bytes read from the ring per send */
#define LOG_SEND_TIMEOUT_S 2

static char s_ring[LOG_RING_SIZE];
static uint32_t s_head; /* total bytes ever written (wraps naturally) */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_orig_vprintf;

/* --- Ring buffer --- */
static void ring_write(const char *data, size_t len)
{
    portENTER_CRITICAL(&s_mux);
    uint32_t idx = s_head & (LOG_RING_SIZE - 1);
    size_t first = LOG_RING_SIZE - idx;
    if (first > len)
    {
        first = len;
    }
    memcpy(&s_ring[idx], data, first);
    if (len > first)
    {
        memcpy(s_ring, data + first, len - first);
    }
    s_head += (uint32_t)len;
    portEXIT_CRITICAL(&s_mux);
}

/* Copy up to max bytes starting at *pos. If the reader fell behind and data
 * was overwritten, *pos jumps forward and *dropped reports how many bytes. */
static size_t ring_read(uint32_t *pos, char *out, size_t max, uint32_t *dropped)
{
    size_t n;
    portENTER_CRITICAL(&s_mux);
    uint32_t behind = s_head - *pos;
    *dropped = 0;
    if (behind > LOG_RING_SIZE)
    {
        *dropped = behind - LOG_RING_SIZE;
        *pos = s_head - LOG_RING_SIZE;
        behind = LOG_RING_SIZE;
    }
    n = behind < max ? behind : max;
    uint32_t idx = *pos & (LOG_RING_SIZE - 1);
    size_t first = LOG_RING_SIZE - idx;
    if (first > n)
    {
        first = n;
    }
    memcpy(out, &s_ring[idx], first);
    if (n > first)
    {
        memcpy(out + first, s_ring, n - first);
    }
    *pos += (uint32_t)n;
    portEXIT_CRITICAL(&s_mux);
    return n;
}

/* Position for a new client: oldest data still in the ring, starting at a line boundary. */
static uint32_t ring_start_pos(void)
{
    portENTER_CRITICAL(&s_mux);
    uint32_t pos = 0;
    if (s_head > LOG_RING_SIZE)
    {
        pos = s_head - LOG_RING_SIZE;
        /* The oldest line is probably cut in half: skip to the next newline */
        while (pos != s_head && s_ring[pos & (LOG_RING_SIZE - 1)] != '\n')
        {
            pos++;
        }
        if (pos != s_head)
        {
            pos++;
        }
    }
    portEXIT_CRITICAL(&s_mux);
    return pos;
}

/* --- vprintf hook --- */
static int log_vprintf(const char *fmt, va_list args)
{
    va_list copy;
    va_copy(copy, args);
    int ret = s_orig_vprintf != NULL ? s_orig_vprintf(fmt, args) : vprintf(fmt, args);

    char line[LOG_LINE_MAX];
    int n = vsnprintf(line, sizeof(line), fmt, copy);
    va_end(copy);
    if (n > 0)
    {
        if (n >= (int)sizeof(line))
        {
            n = sizeof(line) - 1;
            memcpy(line + n - 4, "...\n", 4);
        }
        ring_write(line, (size_t)n);
    }
    return ret;
}

void log_server_install_hook(void)
{
    if (s_orig_vprintf == NULL)
    {
        s_orig_vprintf = esp_log_set_vprintf(log_vprintf);
    }
}

/* --- TCP side --- */
static bool send_all(int sock, const char *data, size_t len)
{
    while (len > 0)
    {
        int w = send(sock, data, len, 0);
        if (w <= 0)
        {
            return false;
        }
        data += w;
        len -= (size_t)w;
    }
    return true;
}

/* Send text converting bare "\n" to "\r\n" for raw terminals. */
static bool send_text(int sock, const char *in, size_t len)
{
    char out[LOG_CHUNK * 2 + 8];
    size_t o = 0;
    static char prev = 0;
    for (size_t i = 0; i < len; i++)
    {
        if (in[i] == '\n' && prev != '\r')
        {
            out[o++] = '\r';
        }
        out[o++] = in[i];
        prev = in[i];
    }
    return send_all(sock, out, o);
}


/* --- On-demand session ---
 * States: IDLE (no socket open) -> LISTENING (port open, waiting for a client,
 * at most LOG_SERVER_WINDOW_S) -> CLIENT (listener already closed, streaming)
 * -> IDLE again as soon as the client disconnects. */
typedef enum
{
    LS_IDLE,
    LS_LISTENING,
    LS_CLIENT,
} ls_state_t;

static ls_state_t s_state = LS_IDLE;
static int64_t s_deadline_us;
static TaskHandle_t s_task;
static portMUX_TYPE s_state_mux = portMUX_INITIALIZER_UNLOCKED;

static void set_state(ls_state_t st)
{
    portENTER_CRITICAL(&s_state_mux);
    s_state = st;
    portEXIT_CRITICAL(&s_state_mux);
}

log_open_result_t log_server_request_open(void)
{
    log_open_result_t res;
    if (s_task == NULL)
    {
        return LOG_OPEN_ERROR;
    }
    portENTER_CRITICAL(&s_state_mux);
    if (s_state == LS_CLIENT)
    {
        res = LOG_OPEN_BUSY;
    }
    else
    {
        s_deadline_us = esp_timer_get_time() + (int64_t)LOG_SERVER_WINDOW_S * 1000000LL;
        if (s_state == LS_LISTENING)
        {
            res = LOG_OPEN_EXTENDED;
        }
        else
        {
            s_state = LS_LISTENING;
            res = LOG_OPEN_STARTED;
        }
    }
    portEXIT_CRITICAL(&s_state_mux);
    if (res == LOG_OPEN_STARTED)
    {
        xTaskNotifyGive(s_task);
    }
    return res;
}

static int open_listener(void)
{
    int ls = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (ls < 0)
    {
        ESP_LOGE(TAG, "socket() failed: errno %d", errno);
        return -1;
    }
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(LOG_SERVER_PORT),
    };
    if (bind(ls, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(ls, 1) != 0)
    {
        ESP_LOGE(TAG, "bind/listen on port %d failed: errno %d", LOG_SERVER_PORT, errno);
        close(ls);
        return -1;
    }
    return ls;
}

/* Wait for one client until the window expires. Returns the client socket or -1. */
static int wait_for_client(int ls)
{
    while (1)
    {
        portENTER_CRITICAL(&s_state_mux);
        int64_t deadline = s_deadline_us; /* may be extended by another request */
        portEXIT_CRITICAL(&s_state_mux);
        if (esp_timer_get_time() >= deadline)
        {
            return -1;
        }
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(ls, &rfds);
        struct timeval tv = {.tv_sec = 0, .tv_usec = 200000};
        if (select(ls + 1, &rfds, NULL, NULL, &tv) > 0 && FD_ISSET(ls, &rfds))
        {
            struct sockaddr_in peer;
            socklen_t plen = sizeof(peer);
            int cs = accept(ls, (struct sockaddr *)&peer, &plen);
            if (cs >= 0)
            {
                ESP_LOGI(TAG, "client connected from " IPSTR, IP2STR((const esp_ip4_addr_t *)&peer.sin_addr.s_addr));
                return cs;
            }
        }
    }
}

static void stream_to_client(int cs)
{
    int one = 1;
    struct timeval snd = {.tv_sec = LOG_SEND_TIMEOUT_S, .tv_usec = 0};
    setsockopt(cs, SOL_SOCKET, SO_SNDTIMEO, &snd, sizeof(snd));
    setsockopt(cs, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    static const char banner[] = "--- Prog Wi-Fi log console (read-only) ---\r\n";
    if (!send_all(cs, banner, sizeof(banner) - 1))
    {
        return;
    }
    uint32_t pos = ring_start_pos();

    while (1)
    {
        /* Ignore anything the client types; detect disconnects */
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(cs, &rfds);
        struct timeval tv = {.tv_sec = 0, .tv_usec = 50000};
        int r = select(cs + 1, &rfds, NULL, NULL, &tv);
        if (r > 0 && FD_ISSET(cs, &rfds))
        {
            char discard[32];
            int k = recv(cs, discard, sizeof(discard), MSG_DONTWAIT);
            if (k == 0 || (k < 0 && errno != EWOULDBLOCK && errno != EAGAIN))
            {
                return;
            }
        }

        /* Stream new log data */
        for (int i = 0; i < 16; i++)
        {
            char chunk[LOG_CHUNK];
            uint32_t dropped = 0;
            size_t n = ring_read(&pos, chunk, sizeof(chunk), &dropped);
            if (dropped > 0)
            {
                char note[48];
                int m = snprintf(note, sizeof(note), "\r\n[... %u bytes dropped ...]\r\n", (unsigned)dropped);
                if (!send_all(cs, note, (size_t)m))
                {
                    return;
                }
            }
            if (n == 0)
            {
                break;
            }
            if (!send_text(cs, chunk, n))
            {
                return;
            }
        }
    }
}

static void log_task(void *arg)
{
    while (1)
    {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY); /* sleep until the endpoint opens the port */

        int ls = open_listener();
        if (ls < 0)
        {
            set_state(LS_IDLE);
            continue;
        }
        ESP_LOGI(TAG, "log console port %d OPEN for up to %d s", LOG_SERVER_PORT, LOG_SERVER_WINDOW_S);

        int cs = wait_for_client(ls);
        close(ls); /* port closed again: after a connection or after the timeout */

        if (cs < 0)
        {
            ESP_LOGI(TAG, "no client connected, port %d CLOSED", LOG_SERVER_PORT);
            set_state(LS_IDLE);
            continue;
        }

        set_state(LS_CLIENT);
        ESP_LOGI(TAG, "port %d CLOSED (single client connected)", LOG_SERVER_PORT);
        stream_to_client(cs);
        close(cs);
        ESP_LOGI(TAG, "client disconnected, session ended");
        set_state(LS_IDLE);
    }
}

void log_server_start(void)
{
    if (s_task != NULL)
    {
        return;
    }
    if (xTaskCreate(log_task, "log_server", 4096, NULL, 3, &s_task) != pdPASS)
    {
        s_task = NULL;
        ESP_LOGE(TAG, "could not create log server task");
    }
}
