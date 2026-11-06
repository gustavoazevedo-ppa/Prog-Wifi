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
#include "lwip/sockets.h"
#include "log_server.h"

#define TAG "LOGSRV"

#define LOG_SERVER_PORT 2323
#define LOG_RING_SIZE 4096 /* must be a power of two */
#define LOG_LINE_MAX 320   /* longer lines are truncated with "..." */
#define LOG_CHUNK 200      /* bytes read from the ring per send */
#define LOG_SEND_TIMEOUT_S 2

static char s_ring[LOG_RING_SIZE];
static uint32_t s_head; /* total bytes ever written (wraps naturally) */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_orig_vprintf;

/* ─── Ring buffer ────────────────────────────────────────────────────────── */
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

/* ─── vprintf hook ───────────────────────────────────────────────────────── */
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

/* ─── TCP side ───────────────────────────────────────────────────────────── */
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

static void close_client(int *cs)
{
    if (*cs >= 0)
    {
        close(*cs);
        *cs = -1;
    }
}

static void log_task(void *arg)
{
    int ls = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (ls < 0)
    {
        ESP_LOGE(TAG, "socket() failed: errno %d", errno);
        vTaskDelete(NULL);
        return;
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
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "log console listening on port %d (read-only, 1 client)", LOG_SERVER_PORT);

    int cs = -1;
    uint32_t pos = 0;
    while (1)
    {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(ls, &rfds);
        int maxfd = ls;
        if (cs >= 0)
        {
            FD_SET(cs, &rfds);
            if (cs > maxfd)
            {
                maxfd = cs;
            }
        }
        struct timeval tv = {.tv_sec = 0, .tv_usec = 50000};
        int r = select(maxfd + 1, &rfds, NULL, NULL, &tv);

        if (r > 0 && FD_ISSET(ls, &rfds))
        {
            struct sockaddr_in peer;
            socklen_t plen = sizeof(peer);
            int ns = accept(ls, (struct sockaddr *)&peer, &plen);
            if (ns >= 0)
            {
                close_client(&cs); /* newest connection wins */
                cs = ns;
                struct timeval snd = {.tv_sec = LOG_SEND_TIMEOUT_S, .tv_usec = 0};
                setsockopt(cs, SOL_SOCKET, SO_SNDTIMEO, &snd, sizeof(snd));
                setsockopt(cs, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
                ESP_LOGI(TAG, "client connected from " IPSTR, IP2STR((const esp_ip4_addr_t *)&peer.sin_addr.s_addr));
                static const char banner[] = "--- Prog Wi-Fi log console (read-only) ---\r\n";
                if (!send_all(cs, banner, sizeof(banner) - 1))
                {
                    close_client(&cs);
                }
                pos = ring_start_pos();
            }
        }

        /* Ignore anything the client types; detect disconnects */
        if (cs >= 0 && r > 0 && FD_ISSET(cs, &rfds))
        {
            char discard[32];
            int k = recv(cs, discard, sizeof(discard), MSG_DONTWAIT);
            if (k == 0 || (k < 0 && errno != EWOULDBLOCK && errno != EAGAIN))
            {
                close_client(&cs);
            }
        }

        /* Stream new log data */
        for (int i = 0; cs >= 0 && i < 16; i++)
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
                    close_client(&cs);
                    break;
                }
            }
            if (n == 0)
            {
                break;
            }
            if (!send_text(cs, chunk, n))
            {
                close_client(&cs);
                ESP_LOGI(TAG, "client disconnected");
            }
        }
    }
}

void log_server_start(void)
{
    if (xTaskCreate(log_task, "log_server", 4096, NULL, 3, NULL) != pdPASS)
    {
        ESP_LOGE(TAG, "could not create log server task");
    }
}
