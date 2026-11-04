#include "monip_client.h"
#include "esp_log.h"
#include "nvs.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "uart_handler.h"
#include "packet_manager.h"
#include <string.h>

#define TAG "MONIP"
// NVS namespace kept as "dc09" for backward compatibility with configs already
// stored on deployed devices — renaming it would orphan saved receiver config.
#define MONIP_NVS_NAMESPACE "dc09"
#define MONIP_TIMEOUT_SEC 5
#define MONIP_RX_TASK_STACK 4096
#define MONIP_RX_TASK_PRIO 4
#define MONIP_RX_BUF_SIZE 128
#define MONIP_CMD_TASK_STACK 4096
#define MONIP_CMD_TASK_PRIO 4
#define MONIP_CMD_QUEUE_LEN 8
#define MONIP_KA_TASK_STACK 4096
#define MONIP_KA_TASK_PRIO 3   // below RX/CMD: keepalive tolerates a little delay

// Connection state per receiver (internal)
typedef struct {
    char ip[33];
    uint16_t port;
    int sock;
    bool connected;

    // Transaction serialization: guarantees only ONE '@'-expecting operation
    // (keepalive or event send) is outstanding on this socket at a time, so an
    // incoming '@' is unambiguously the ACK for the current waiter.
    SemaphoreHandle_t txn_mutex;
    TaskHandle_t waiter;        // task currently waiting for '@' ACK (NULL if none)
    volatile bool ack_received; // set by RX task when '@' is routed to the waiter

    // Monip v2 state machine (per receiver, independent of the other).
    monip_state_t state;         // current connection state
    uint8_t bit;                // MONIP_DEST_A or MONIP_DEST_B (identity)
    uint8_t ka_fail_count;      // consecutive keepalive failures
    const char *tag;            // "A" or "B" for logs

    // Per-receiver fd lifecycle lock. Protects open/close/assignment of `sock`
    // for THIS receiver only. Per-receiver (not shared) so a slow connect or
    // handshake on one receiver cannot block the RX task from reading the other.
    SemaphoreHandle_t life_mutex;

    // Set by a CMD 0x38 (disconnect) or CMD 0x30 (connect) that arrives while
    // this receiver is mid-reconnect on the keepalive task. The reconnect
    // clobbers `state` to CONNECTED on success, erasing the evidence that a
    // disconnect was requested — so we track the request in a dedicated flag
    // that monip_try_reconnect_one checks (under life_mutex) after connecting,
    // to tear down a session the HOST no longer wants.
    volatile bool cancel_reconnect;
} monip_receiver_t;

static monip_receiver_t s_recv_a = { .sock = -1, .connected = false,
                                     .state = MONIP_STATE_IDLE,
                                     .bit = MONIP_DEST_A, .tag = "A" };
static monip_receiver_t s_recv_b = { .sock = -1, .connected = false,
                                     .state = MONIP_STATE_IDLE,
                                     .bit = MONIP_DEST_B, .tag = "B" };

static char s_acct[5] = {0};
static char s_imei[7] = {0};
static char s_conn_type = 'E';

// Autonomous keepalive interval (seconds), loaded from NVS. 0 => default 60s.
static uint16_t s_keepalive_sec = 60;

// Internal reconnection tuning (spec 4G section 7).
#define MONIP_KA_FAIL_THRESHOLD 3   // consecutive KA failures before RECONNECTING
#define MONIP_RECONNECT_ATTEMPTS 3  // internal reconnect tries before giving up
#define MONIP_RECONNECT_DELAY_MS 10000

// Helper: log + set a receiver's state.
static void monip_set_state(monip_receiver_t *r, monip_state_t st)
{
    if (r->state != st)
    {
        static const char *names[] = { "IDLE", "CONNECTING", "CONNECTED",
                                       "RECONNECTING", "DISCONNECTED" };
        ESP_LOGI(TAG, "Receiver %s: %s -> %s", r->tag, names[r->state], names[st]);
        r->state = st;
    }
}

static QueueHandle_t s_cmd_queue = NULL;
static bool s_rx_task_started = false;
static bool s_cmd_task_started = false;
static bool s_ka_task_started = false;

static void monip_rx_task(void *arg);
static void monip_cmd_task(void *arg);
static void monip_keepalive_task(void *arg);
static void monip_send_cmd34(const uint8_t *data, size_t len);
static bool monip_txn_send_expect_ack(monip_receiver_t *r,
                                     const void *payload, size_t len,
                                     uint32_t timeout_ms);
static void monip_send_status_change(uint8_t status);

// --- Static blocking helpers (run only in monip_cmd_task context) ---

static int monip_open_socket(const char *host, uint16_t port)
{
    struct sockaddr_in dest_addr = {0};
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(port);

    int ret = inet_pton(AF_INET, host, &dest_addr.sin_addr);
    if (ret != 1)
    {
        struct addrinfo hints = {0};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        struct addrinfo *res = NULL;
        char port_str[6];
        snprintf(port_str, sizeof(port_str), "%u", port);

        int dns_err = getaddrinfo(host, port_str, &hints, &res);
        if (dns_err != 0 || res == NULL)
        {
            ESP_LOGE(TAG, "DNS resolution failed for '%s': %d", host, dns_err);
            return -1;
        }
        memcpy(&dest_addr, res->ai_addr, sizeof(dest_addr));
        freeaddrinfo(res);
    }

    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0)
    {
        ESP_LOGE(TAG, "Socket creation failed: errno %d", errno);
        return -1;
    }

    struct timeval tv = { .tv_sec = MONIP_TIMEOUT_SEC, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    ret = connect(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (ret != 0)
    {
        ESP_LOGE(TAG, "TCP connect to %s:%u failed: errno %d", host, port, errno);
        closesocket(sock);
        return -1;
    }

    ESP_LOGI(TAG, "TCP connected to %s:%u (fd=%d)", host, port, sock);
    return sock;
}

static bool monip_handshake(int sock)
{
    char buf[16];
    int len;

    len = snprintf(buf, sizeof(buf), "*%s", s_imei);
    if (send(sock, buf, len, 0) < 0)
    {
        ESP_LOGE(TAG, "Failed to send IMEI");
        return false;
    }

    len = recv(sock, buf, sizeof(buf) - 1, 0);
    if (len <= 0)
    {
        ESP_LOGE(TAG, "No response to IMEI (len=%d, errno=%d)", len, errno);
        return false;
    }
    buf[len] = '\0';

    if (buf[0] == '-')
    {
        ESP_LOGW(TAG, "Receiver rejected IMEI");
        return false;
    }
    if (buf[0] != '+')
    {
        ESP_LOGW(TAG, "Unexpected IMEI response: '%s'", buf);
        return false;
    }

    len = snprintf(buf, sizeof(buf), "#%s%c", s_acct, s_conn_type);
    if (send(sock, buf, len, 0) < 0)
    {
        ESP_LOGE(TAG, "Failed to send ACCT");
        return false;
    }

    len = recv(sock, buf, sizeof(buf) - 1, 0);
    if (len <= 0)
    {
        ESP_LOGE(TAG, "No response to ACCT (len=%d, errno=%d)", len, errno);
        return false;
    }
    buf[len] = '\0';

    if (buf[0] != '@')
    {
        ESP_LOGW(TAG, "Unexpected ACCT response: '%s'", buf);
        return false;
    }

    ESP_LOGI(TAG, "Handshake successful");
    return true;
}

static uint8_t monip_connect_one(monip_receiver_t *recv, uint8_t bit)
{
    // 13.1: CONNECT while already CONNECTED is a no-op. The MONIP receiver
    // accepts only one TCP session per account; re-handshaking would kill the
    // good session and trigger "IMEI rejected". Report success, keep session.
    if (recv->state == MONIP_STATE_CONNECTED && recv->connected && recv->sock >= 0)
    {
        ESP_LOGI(TAG, "Receiver %s already CONNECTED, ignoring CONNECT", recv->tag);
        return bit;
    }

    // 13.2: always close any prior socket before a new handshake to avoid a
    // phantom session on the receiver that would refuse the new connection.
    if (recv->sock >= 0)
    {
        closesocket(recv->sock);
        recv->sock = -1;
        recv->connected = false;
    }

    if (recv->ip[0] == '\0' || recv->port == 0)
    {
        ESP_LOGW(TAG, "Receiver %s not configured", recv->tag);
        monip_set_state(recv, MONIP_STATE_IDLE);
        return 0;
    }

    monip_set_state(recv, MONIP_STATE_CONNECTING);

    recv->sock = monip_open_socket(recv->ip, recv->port);
    if (recv->sock < 0)
    {
        monip_set_state(recv, MONIP_STATE_IDLE);
        return 0;
    }

    if (!monip_handshake(recv->sock))
    {
        closesocket(recv->sock);
        recv->sock = -1;
        monip_set_state(recv, MONIP_STATE_IDLE);
        return 0;
    }

    recv->connected = true;
    recv->ka_fail_count = 0;
    monip_set_state(recv, MONIP_STATE_CONNECTED);
    return bit;
}

static uint8_t monip_connect_internal(uint8_t dest)
{
    uint8_t status = 0;

    // Each receiver connects under ITS OWN life_mutex. B's (possibly slow) TCP
    // connect + handshake only holds B's lock, so it can't block the RX task
    // from snapshotting and reading A's socket.
    if (dest & MONIP_DEST_A)
    {
        ESP_LOGI(TAG, "Connecting to receiver A (%s:%u)", s_recv_a.ip, s_recv_a.port);
        xSemaphoreTake(s_recv_a.life_mutex, portMAX_DELAY);
        status |= monip_connect_one(&s_recv_a, MONIP_DEST_A);
        xSemaphoreGive(s_recv_a.life_mutex);
    }

    if (dest & MONIP_DEST_B)
    {
        ESP_LOGI(TAG, "Connecting to receiver B (%s:%u)", s_recv_b.ip, s_recv_b.port);
        xSemaphoreTake(s_recv_b.life_mutex, portMAX_DELAY);
        status |= monip_connect_one(&s_recv_b, MONIP_DEST_B);
        xSemaphoreGive(s_recv_b.life_mutex);
    }

    ESP_LOGI(TAG, "monip_connect result: 0x%02X", status);
    return status;
}

// Send CMD 0x40 (STATUS_CHANGE) to the HOST over UART. `status` is 0x00 =
// disconnected (4G HOST convention). Frame matches the other MONIP UART
// responses: AA AA 00 01 03 40 [status] [crc] BB BB, CRC over CMD + status.
// TODO: Identify the channel A/B in the status byte once the HOST supports it.
static void monip_send_status_change(uint8_t status)
{
    uint8_t response[] = {0xAA, 0xAA, 0x00, 0x01, 0x03, 0x40,
                          status, 0x00, 0xBB, 0xBB};
    response[7] = crc8(&response[5], 2); // CRC over CMD + status
    uart_handler_write_bytes(response, sizeof(response));
    ESP_LOGI(TAG, "CMD_40 STATUS_CHANGE sent: status=0x%02X", status);
}

// Internal reconnection for one receiver (spec 4G section 7). Runs entirely
// inside the keepalive task and is transparent to the HOST: on success the
// receiver returns to CONNECTED with no serial notification. On exhaustion the
// receiver goes DISCONNECTED (HOST notification via CMD 0x40 is a later phase).
//
// Honors a commanded stop: if another task moves the receiver to IDLE (CMD 0x38
// DISCONNECT) or re-CONNECT, we abort the reconnect loop.
static void monip_try_reconnect_one(monip_receiver_t *r)
{
    // Fresh recovery cycle: clear any stale cancellation request. From here on,
    // any CMD 0x38 (disconnect) that sets cancel_reconnect will be honored after
    // the next connect attempt, tearing down a session the HOST no longer wants.
    xSemaphoreTake(r->life_mutex, portMAX_DELAY);
    r->cancel_reconnect = false;
    xSemaphoreGive(r->life_mutex);

    monip_set_state(r, MONIP_STATE_RECONNECTING);

    for (int attempt = 0; attempt < MONIP_RECONNECT_ATTEMPTS; attempt++)
    {
        // Abort if a serial command changed our intent mid-recovery.
        if (r->state != MONIP_STATE_RECONNECTING)
        {
            ESP_LOGI(TAG, "Receiver %s: reconnect aborted (state changed)", r->tag);
            return;
        }

        vTaskDelay(pdMS_TO_TICKS(MONIP_RECONNECT_DELAY_MS));

        if (r->state != MONIP_STATE_RECONNECTING)
        {
            ESP_LOGI(TAG, "Receiver %s: reconnect aborted (state changed)", r->tag);
            return;
        }

        ESP_LOGI(TAG, "Receiver %s: reconnect attempt %d/%d",
                 r->tag, attempt + 1, MONIP_RECONNECT_ATTEMPTS);

        // monip_connect_one closes the old socket first (13.2) and sets state
        // to CONNECTING then CONNECTED/IDLE. Serialize with connect/disconnect
        // for THIS receiver only (per-receiver lock keeps the other independent).
        //
        // connect and the cancel-check happen inside ONE lock hold, atomic w.r.t.
        // a concurrent CMD 0x38 (which takes the same lock and sets
        // cancel_reconnect). cancel_reconnect was cleared once at the start of
        // this recovery cycle; any disconnect since then (including one that ran
        // between the loop-top state check and acquiring this lock) leaves it
        // set, and the post-connect check below honors it authoritatively.
        xSemaphoreTake(r->life_mutex, portMAX_DELAY);
        // Force a fresh handshake: clear the CONNECTED short-circuit (13.1) by
        // ensuring state isn't CONNECTED before calling (it's RECONNECTING here).
        uint8_t ok = monip_connect_one(r, r->bit);

        // If a disconnect was requested (flag set before we took the lock, or a
        // CMD 0x38 is queued behind us), honor it: tear down the session we just
        // brought up so we don't leave a phantom connection the HOST doesn't want.
        if (ok && r->cancel_reconnect)
        {
            ESP_LOGW(TAG, "Receiver %s: reconnect superseded by disconnect, tearing down", r->tag);
            if (r->sock >= 0)
            {
                closesocket(r->sock);
                r->sock = -1;
                r->connected = false;
            }
            monip_set_state(r, MONIP_STATE_IDLE);
            r->cancel_reconnect = false;
            xSemaphoreGive(r->life_mutex);
            return;
        }
        xSemaphoreGive(r->life_mutex);

        if (ok)
        {
            // monip_connect_one already set state CONNECTED and cleared fails.
            ESP_LOGI(TAG, "Receiver %s: reconnected (transparent to HOST)", r->tag);
            return;
        }

        // connect_one set state to IDLE on failure; put it back to RECONNECTING
        // so the loop guard above stays valid for the next attempt.
        if (r->state == MONIP_STATE_IDLE)
            monip_set_state(r, MONIP_STATE_RECONNECTING);
    }

    ESP_LOGW(TAG, "Receiver %s: reconnect exhausted -> DISCONNECTED", r->tag);
    monip_set_state(r, MONIP_STATE_DISCONNECTED);

    // Notify the HOST that this receiver is down after internal recovery failed
    // (CMD 0x40 STATUS_CHANGE). Sending literal 0x00 to match the 4G HOST
    // convention (0x00 = disconnected) for now.
    // TODO: Identify the channel A/B (send r->bit once the HOST parses it).
    monip_send_status_change(0x00);
}

// Service one receiver's keepalive: send '@', track failures, trigger recovery.
static void monip_keepalive_service_one(monip_receiver_t *r)
{
    if (r->state != MONIP_STATE_CONNECTED)
        return;

    if (monip_txn_send_expect_ack(r, "@", 1, MONIP_TIMEOUT_SEC * 1000))
    {
        if (r->ka_fail_count != 0)
            ESP_LOGI(TAG, "Receiver %s: keepalive OK (recovered)", r->tag);
        r->ka_fail_count = 0;
        return;
    }

    r->ka_fail_count++;
    ESP_LOGW(TAG, "Receiver %s: keepalive fail, count=%u",
             r->tag, r->ka_fail_count);

    if (r->ka_fail_count >= MONIP_KA_FAIL_THRESHOLD)
    {
        monip_try_reconnect_one(r);
    }
}

// Autonomous keepalive task. A single task services both receivers, but each
// receiver keeps its OWN timer, fail counter and state — A and B are fully
// independent. We wake on a coarse 1s tick and only send '@' for a receiver
// once its individual interval has elapsed.
//
// `*elapsed` is this receiver's independent seconds-since-last-keepalive counter.
static void monip_keepalive_tick_one(monip_receiver_t *r, uint32_t *elapsed,
                                    uint32_t interval)
{
    if (r->state != MONIP_STATE_CONNECTED)
    {
        *elapsed = 0; // reset timer when not connected
        return;
    }

    // Fast path: the RX task detected a dead socket (recv error / peer close)
    // and cleared `connected` while state was still CONNECTED. Don't wait out
    // 3 keepalive intervals — begin recovery now (socket-close is the cleanest,
    // fastest disconnect signal we get).
    if (!r->connected || r->sock < 0)
    {
        ESP_LOGW(TAG, "Receiver %s: socket dropped, starting recovery now", r->tag);
        *elapsed = 0;
        r->ka_fail_count = 0;
        monip_try_reconnect_one(r);
        return;
    }

    if (++(*elapsed) >= interval)
    {
        *elapsed = 0;
        monip_keepalive_service_one(r);
    }
}

static void monip_keepalive_task(void *arg)
{
    uint32_t elapsed_a = 0;
    uint32_t elapsed_b = 0;

    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));

        uint32_t interval = (s_keepalive_sec > 0) ? s_keepalive_sec : 60;

        monip_keepalive_tick_one(&s_recv_a, &elapsed_a, interval);
        monip_keepalive_tick_one(&s_recv_b, &elapsed_b, interval);
    }
}

// --- Transaction primitive: send + wait for '@' ACK ---
//
// Sends `payload` on the receiver's socket and blocks until the RX task routes
// an '@' ACK back to this task, or `timeout_ms` elapses. The per-receiver
// txn_mutex is held for the whole send->wait cycle, so only one '@'-expecting
// operation can be outstanding at a time — this is what lets a bare '@' be
// correlated to its request without a sequence number.
//
// Returns true if the ACK arrived within the timeout. On send failure the
// receiver is marked disconnected and false is returned.
static bool monip_txn_send_expect_ack(monip_receiver_t *r,
                                     const void *payload, size_t len,
                                     uint32_t timeout_ms)
{
    if (r->sock < 0 || !r->connected)
    {
        ESP_LOGW(TAG, "txn: receiver not connected");
        return false;
    }

    xSemaphoreTake(r->txn_mutex, portMAX_DELAY);

    // Register as the waiter BEFORE sending so the RX task cannot signal an
    // ACK that we then miss. Clear any stale notification first.
    r->ack_received = false;
    r->waiter = xTaskGetCurrentTaskHandle();
    ulTaskNotifyValueClear(NULL, 0);

    int sent = send(r->sock, payload, len, 0);
    if (sent < 0)
    {
        ESP_LOGE(TAG, "txn: send failed (errno %d)", errno);
        r->waiter = NULL;
        r->connected = false; // RX task owns the actual close on recv<=0
        xSemaphoreGive(r->txn_mutex);
        return false;
    }

    // Block until the RX task notifies us (ACK routed) or we time out.
    uint32_t n = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(timeout_ms));

    r->waiter = NULL;
    bool ok = (n > 0 && r->ack_received);

    if (!ok)
    {
        ESP_LOGW(TAG, "txn: no '@' ACK within %u ms", (unsigned)timeout_ms);
    }

    xSemaphoreGive(r->txn_mutex);
    return ok;
}

// CMD 0x32 (HOST-driven keepalive query) is retained for backward compatibility
// with a v1 HOST, but in Monip v2 the module keeps the socket alive on its own
// (monip_keepalive_task). So this handler does NOT send '@' on the socket — doing
// so would duplicate the autonomous keepalive traffic and let the CMD 0x32 path
// perturb the state machine. Instead it answers the HOST's query honestly from
// the current per-receiver state: a receiver is reported "alive" only while it
// is CONNECTED. The result is returned as CMD 0x33 by the caller.
static uint8_t monip_keepalive_internal(uint8_t dest)
{
    uint8_t status = 0;

    if (dest & MONIP_DEST_A)
    {
        if (s_recv_a.state == MONIP_STATE_CONNECTED && s_recv_a.connected)
            status |= MONIP_DEST_A;
    }

    if (dest & MONIP_DEST_B)
    {
        if (s_recv_b.state == MONIP_STATE_CONNECTED && s_recv_b.connected)
            status |= MONIP_DEST_B;
    }

    ESP_LOGI(TAG, "CMD_32 query answered from state: result=0x%02X", status);
    return status;
}

// Encode a hex digit for Contact ID (A-F become :;<=>?)
static char monip_encode_cid_digit(int digit)
{
    if (digit >= 0 && digit <= 9)
        return '0' + digit;
    return ':' + (digit - 10);
}

// Parse an ASCII hex character ('0'-'9', 'A'-'F', 'a'-'f') to its numeric value 0-15
static int monip_parse_hex_char(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return 0; // fallback for invalid input
}

static uint8_t monip_send_event_one(monip_receiver_t *receiver, const uint8_t *payload, uint8_t payload_len)
{
    if (receiver->sock < 0 || !receiver->connected)
    {
        ESP_LOGW(TAG, "send_event: not connected");
        return 0;
    }

    if (payload_len < 10)
    {
        ESP_LOGW(TAG, "send_event: payload too short (%d < 10)", payload_len);
        return 0;
    }

    char acct[5] = {0};
    memcpy(acct, &payload[0], 4);

    char qualifier = (char)payload[4];

    uint16_t event_code = ((uint16_t)payload[5] << 8) | payload[6];
    uint8_t partition = payload[7];
    uint16_t zone_user = ((uint16_t)payload[8] << 8) | payload[9];

    char cid[18];
    int x = (event_code / 100) % 10;
    int y = (event_code / 10) % 10;
    int z = event_code % 10;
    int g1 = (partition / 10) % 10;
    int g2 = partition % 10;
    int c1 = (zone_user / 100) % 10;
    int c2 = (zone_user / 10) % 10;
    int c3 = zone_user % 10;

    int n = snprintf(cid, sizeof(cid), "%%%c%c%c%c%c%c%c%c%c%c%c%c%c0%c",
                     monip_encode_cid_digit(monip_parse_hex_char(acct[0])),
                     monip_encode_cid_digit(monip_parse_hex_char(acct[1])),
                     monip_encode_cid_digit(monip_parse_hex_char(acct[2])),
                     monip_encode_cid_digit(monip_parse_hex_char(acct[3])),
                     qualifier,
                     monip_encode_cid_digit(x),
                     monip_encode_cid_digit(y),
                     monip_encode_cid_digit(z),
                     monip_encode_cid_digit(g1),
                     monip_encode_cid_digit(g2),
                     monip_encode_cid_digit(c1),
                     monip_encode_cid_digit(c2),
                     monip_encode_cid_digit(c3),
                     (char)0xB6);

    ESP_LOGI(TAG, "CID string: '%.*s' (%d bytes)", n, cid, n);

    if (monip_txn_send_expect_ack(receiver, cid, n, MONIP_TIMEOUT_SEC * 1000))
    {
        ESP_LOGI(TAG, "send_event: ACK received");
        return 1;
    }

    ESP_LOGW(TAG, "send_event: no ACK");
    return 0;
}

static uint8_t monip_send_event_internal(uint8_t dest, const uint8_t *payload, uint8_t payload_len)
{
    uint8_t status = 0;

    // No lifecycle lock here: monip_txn_send_expect_ack serializes per-receiver
    // via each receiver's txn_mutex, and the RX task is the sole reader.

    if (dest & MONIP_DEST_A)
    {
        if (monip_send_event_one(&s_recv_a, payload, payload_len))
            status |= MONIP_DEST_A;
    }

    if (dest & MONIP_DEST_B)
    {
        if (monip_send_event_one(&s_recv_b, payload, payload_len))
            status |= MONIP_DEST_B;
    }

    ESP_LOGI(TAG, "MONIP_send_event result: 0x%02X", status);
    return status;
}

// Fire-and-forget write to a receiver (used for CMD 0x35 responses). Any reply
// the receiver sends comes back asynchronously through the RX task, so we don't
// wait for an ACK here. The txn_mutex is held only for the brief send() to keep
// writes from interleaving with a concurrent keepalive/event send.
static bool monip_forward_one(monip_receiver_t *r, const uint8_t *data, size_t len)
{
    bool ok = false;

    if (r->sock < 0 || !r->connected)
        return false;

    xSemaphoreTake(r->txn_mutex, portMAX_DELAY);
    if (send(r->sock, data, len, 0) >= 0)
    {
        ok = true;
        ESP_LOGI(TAG, "Forwarded %d bytes to receiver", (int)len);
    }
    else
    {
        ESP_LOGE(TAG, "Forward failed (errno %d)", errno);
    }
    xSemaphoreGive(r->txn_mutex);

    return ok;
}

static bool monip_forward_internal(uint8_t dest, const uint8_t *data, size_t len)
{
    bool ok = false;

    if (dest & MONIP_DEST_A)
        ok |= monip_forward_one(&s_recv_a, data, len);

    if (dest & MONIP_DEST_B)
        ok |= monip_forward_one(&s_recv_b, data, len);

    return ok;
}

static void monip_disconnect_internal(uint8_t dest)
{
    if (dest & MONIP_DEST_A)
    {
        xSemaphoreTake(s_recv_a.life_mutex, portMAX_DELAY);
        if (s_recv_a.sock >= 0)
        {
            closesocket(s_recv_a.sock);
            s_recv_a.sock = -1;
            s_recv_a.connected = false;
            ESP_LOGI(TAG, "Receiver A disconnected");
        }
        // Commanded disconnect: go IDLE so keepalive stops and no reconnect runs.
        // Also flag cancellation in case a reconnect is in-flight on the KA task
        // (if it holds the lock now, we run after it and this close undoes its
        //  work; if it runs after us, it will see the flag and tear down).
        monip_set_state(&s_recv_a, MONIP_STATE_IDLE);
        s_recv_a.ka_fail_count = 0;
        s_recv_a.cancel_reconnect = true;
        xSemaphoreGive(s_recv_a.life_mutex);
    }

    if (dest & MONIP_DEST_B)
    {
        xSemaphoreTake(s_recv_b.life_mutex, portMAX_DELAY);
        if (s_recv_b.sock >= 0)
        {
            closesocket(s_recv_b.sock);
            s_recv_b.sock = -1;
            s_recv_b.connected = false;
            ESP_LOGI(TAG, "Receiver B disconnected");
        }
        monip_set_state(&s_recv_b, MONIP_STATE_IDLE);
        s_recv_b.ka_fail_count = 0;
        s_recv_b.cancel_reconnect = true;
        xSemaphoreGive(s_recv_b.life_mutex);
    }   
}

// --- Command task: dequeues commands and runs blocking I/O ---

static void monip_cmd_task(void *arg)
{
    monip_cmd_t cmd;

    for (;;)
    {
        if (xQueueReceive(s_cmd_queue, &cmd, portMAX_DELAY) != pdTRUE)
            continue;

        switch (cmd.type)
        {
        case MONIP_CMD_CONNECT:
        {
            uint8_t status = monip_connect_internal(cmd.dest);
            // Send CMD_31 response to UART (2-byte length field)
            uint8_t response[] = {0xAA, 0xAA, 0x00, 0x01, 0x03, 0x31,
                                  status, 0x00, 0xBB, 0xBB};
            response[7] = crc8(&response[5], 2); // CRC over CMD + status
            uart_handler_write_bytes(response, sizeof(response));
            ESP_LOGI(TAG, "CMD_31 response sent: status=0x%02X", status);
            break;
        }
        case MONIP_CMD_KEEPALIVE:
        {
            uint8_t status = monip_keepalive_internal(cmd.dest);
            // Send CMD_33 response to UART (2-byte length field)
            uint8_t response[] = {0xAA, 0xAA, 0x00, 0x01, 0x03, 0x33,
                                  status, 0x00, 0xBB, 0xBB};
            response[7] = crc8(&response[5], 2); // CRC over CMD + status
            uart_handler_write_bytes(response, sizeof(response));
            ESP_LOGI(TAG, "CMD_33 response sent: status=0x%02X", status);
            break;
        }

        case MONIP_CMD_SEND_EVENT:
        {
            uint8_t status = monip_send_event_internal(cmd.dest, cmd.payload, cmd.payload_len);
            // Respond with same origin CMD + status (2-byte length field)
            uint8_t response[] = {0xAA, 0xAA, 0x00, 0x01, 0x03, cmd.origin_cmd,
                                  status, 0x00, 0xBB, 0xBB};
            response[7] = crc8(&response[5], 2); // CRC over CMD + status
            uart_handler_write_bytes(response, sizeof(response));
            ESP_LOGI(TAG, "CMD_0x%02X response sent: status=0x%02X", cmd.origin_cmd, status);
            break;
        }
        case MONIP_CMD_FORWARD:
        {
            monip_forward_internal(cmd.dest, cmd.payload, cmd.payload_len);
            break;
        }
        case MONIP_CMD_DISCONNECT:
        {
            monip_disconnect_internal(cmd.dest);
            uint8_t status = 0x03;
            // Send CMD_38 response to UART (1-byte length field)
            uint8_t response[] = {0xAA, 0xAA, 0x00, 0x01, 0x03, 0x38,
                                  status, 0x00, 0xBB, 0xBB};
            response[7] = crc8(&response[5], 2); // CRC over CMD + status
            uart_handler_write_bytes(response, sizeof(response));
            ESP_LOGI(TAG, "CMD_38 response sent: status=0x%02X", status);
            break;

            break;
        }
        }

        // Free payload if allocated
        if (cmd.payload)
        {
            vPortFree(cmd.payload);
        }
    }
}

// --- Local command interception ---

// Try to handle a command locally (without forwarding to HOST).
// Returns true if command was handled, false if it should be forwarded.
static bool monip_handle_local_cmd(int sock, const uint8_t *data, size_t len)
{
    // WKOC — Connection type query
    if (len == 4 && memcmp(data, "WKOC", 4) == 0)
    {
        const char *resp = "KE";
        send(sock, resp, 2, 0);
        ESP_LOGI(TAG, "Local reply to WKOC: KE");
        return true;
    }

    return false;
}

// Route one message received from a receiver, classifying it by type. This is
// the demultiplexer that lets keepalive/event ACKs coexist with unsolicited
// receiver-initiated commands on the same full-duplex socket.
static void monip_route_message(monip_receiver_t *r, uint8_t *data, size_t len)
{
    if (len == 0)
        return;

    // 1. Keepalive / event ACK — hand to the operation currently waiting.
    if (data[0] == '@')
    {
        TaskHandle_t w = r->waiter;
        if (w != NULL)
        {
            r->ack_received = true;
            xTaskNotifyGive(w); // wake monip_txn_send_expect_ack
        }
        else
        {
            // '@' arrived with no outstanding request — stray/late ACK. Drop it
            // instead of letting it corrupt the next transaction.
            ESP_LOGW(TAG, "Stray '@' from receiver (no waiter), discarding");
        }
        return;
    }

    // 2. Receiver-initiated command (WA=arm, WE=status, WKOC=conn query, ...).
    if (data[0] == 'W')
    {
        if (monip_handle_local_cmd(r->sock, data, len))
            return;
        monip_send_cmd34(data, len); // forward to HOST
        return;
    }

    // 3. Unknown format — log and discard. Never mistaken for an ACK.
    ESP_LOGW(TAG, "Unknown %d bytes from receiver, discarding", (int)len);
}

// --- RX Task: listens for unsolicited data from receptors ---

static void monip_send_cmd34(const uint8_t *data, size_t len)
{
    // Packet: AA AA LEN_H LEN_L TYPE CMD [DATA...] CRC BB BB
    size_t pkt_size = 2 + 2 + 1 + 1 + len + 1 + 2; // header(2) + len(2) + type(1) + cmd(1) + data + crc(1) + tail(2)
    uint8_t *pkt = pvPortMalloc(pkt_size);
    if (pkt == NULL)
    {
        ESP_LOGE(TAG, "CMD34: malloc failed");
        return;
    }

    pkt[0] = 0xAA;
    pkt[1] = 0xAA;
    pkt[2] = (uint8_t)(len >> 8);   // length high byte
    pkt[3] = (uint8_t)(len & 0xFF); // length low byte
    pkt[4] = 0x03;                  // type = Contact ID
    pkt[5] = 0x34;                  // cmd
    memcpy(&pkt[6], data, len);
    pkt[6 + len] = crc8(&pkt[5], len + 1); // CRC over CMD + data
    pkt[6 + len + 1] = 0xBB;
    pkt[6 + len + 2] = 0xBB;

    uart_handler_write_bytes(pkt, pkt_size);
    ESP_LOGI(TAG, "CMD34 sent to UART (%d bytes payload)", (int)len);

    vPortFree(pkt);
}

static void monip_rx_task(void *arg)
{
    uint8_t rx_buf[MONIP_RX_BUF_SIZE];

    // The RX task is the SOLE reader of the receiver sockets. lwIP permits a
    // concurrent send() (from a keepalive/event transaction) and recv() on the
    // same socket, so writes don't need to coordinate with this reader.
    //
    // The one thing that DOES race is the socket lifecycle: connect/disconnect
    // may close and reopen an fd. To avoid select()/recv() on a stale or reused
    // fd, we snapshot each fd under that receiver's OWN life_mutex, release it,
    // and only then run the (blocking) select()/recv() on the snapshot. Because
    // the locks are per-receiver, a slow connect on B (holding B's life_mutex)
    // does NOT delay snapshotting A — so A's incoming commands keep flowing.
    // The two locks are taken separately (never nested), so no deadlock risk.
    while (1)
    {
        int fd_a = -1;
        int fd_b = -1;

        // Non-blocking snapshot: if a receiver's life_mutex is currently held
        // (e.g. a connect/handshake in progress), skip that receiver THIS
        // iteration rather than blocking. This is what keeps a slow connect on
        // one receiver from stalling reads on the other — we simply re-snapshot
        // on the next 200ms cycle. A busy lock means that fd is mid-transition
        // anyway, so there's nothing useful to read from it right now.
        if (xSemaphoreTake(s_recv_a.life_mutex, 0) == pdTRUE)
        {
            if (s_recv_a.sock >= 0 && s_recv_a.connected) fd_a = s_recv_a.sock;
            xSemaphoreGive(s_recv_a.life_mutex);
        }

        if (xSemaphoreTake(s_recv_b.life_mutex, 0) == pdTRUE)
        {
            if (s_recv_b.sock >= 0 && s_recv_b.connected) fd_b = s_recv_b.sock;
            xSemaphoreGive(s_recv_b.life_mutex);
        }

        if (fd_a < 0 && fd_b < 0)
        {
            // If a receiver is connected but we just couldn't grab its lock
            // (transient: connect/close in progress), re-poll quickly. If truly
            // nothing is connected, back off longer to avoid a busy idle loop.
            bool any_connected = s_recv_a.connected || s_recv_b.connected;
            vTaskDelay(pdMS_TO_TICKS(any_connected ? 20 : 500));
            continue;
        }

        fd_set read_fds;
        FD_ZERO(&read_fds);
        int max_fd = -1;
        if (fd_a >= 0) { FD_SET(fd_a, &read_fds); if (fd_a > max_fd) max_fd = fd_a; }
        if (fd_b >= 0) { FD_SET(fd_b, &read_fds); if (fd_b > max_fd) max_fd = fd_b; }

        struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };
        int sel = select(max_fd + 1, &read_fds, NULL, NULL, &tv);

        if (sel > 0)
        {
            if (fd_a >= 0 && FD_ISSET(fd_a, &read_fds))
            {
                int len = recv(fd_a, rx_buf, sizeof(rx_buf), 0);
                if (len > 0)
                {
                    ESP_LOGI(TAG, "RX from receiver A: %d bytes", len);
                    monip_route_message(&s_recv_a, rx_buf, len);
                }
                else
                {
                    ESP_LOGW(TAG, "Receiver A socket error/closed (len=%d)", len);
                    xSemaphoreTake(s_recv_a.life_mutex, portMAX_DELAY);
                    if (s_recv_a.sock == fd_a) // still the same fd we read from
                    {
                        closesocket(s_recv_a.sock);
                        s_recv_a.sock = -1;
                        s_recv_a.connected = false;
                    }
                    xSemaphoreGive(s_recv_a.life_mutex);
                    // Wake any waiter so it fails fast instead of timing out.
                    if (s_recv_a.waiter != NULL)
                        xTaskNotifyGive(s_recv_a.waiter);
                }
            }

            if (fd_b >= 0 && FD_ISSET(fd_b, &read_fds))
            {
                int len = recv(fd_b, rx_buf, sizeof(rx_buf), 0);
                if (len > 0)
                {
                    ESP_LOGI(TAG, "RX from receiver B: %d bytes", len);
                    monip_route_message(&s_recv_b, rx_buf, len);
                }
                else
                {
                    ESP_LOGW(TAG, "Receiver B socket error/closed (len=%d)", len);
                    xSemaphoreTake(s_recv_b.life_mutex, portMAX_DELAY);
                    if (s_recv_b.sock == fd_b) // still the same fd we read from
                    {
                        closesocket(s_recv_b.sock);
                        s_recv_b.sock = -1;
                        s_recv_b.connected = false;
                    }
                    xSemaphoreGive(s_recv_b.life_mutex);
                    // Wake any waiter so it fails fast instead of timing out.
                    if (s_recv_b.waiter != NULL)
                        xTaskNotifyGive(s_recv_b.waiter);
                }
            }
        }
    }
}

// --- Public API ---

void monip_init(void)
{
    if (s_recv_a.txn_mutex == NULL)
    {
        s_recv_a.txn_mutex = xSemaphoreCreateMutex();
    }

    if (s_recv_b.txn_mutex == NULL)
    {
        s_recv_b.txn_mutex = xSemaphoreCreateMutex();
    }

    if (s_recv_a.life_mutex == NULL)
    {
        s_recv_a.life_mutex = xSemaphoreCreateMutex();
    }

    if (s_recv_b.life_mutex == NULL)
    {
        s_recv_b.life_mutex = xSemaphoreCreateMutex();
    }

    if (s_cmd_queue == NULL)
    {
        s_cmd_queue = xQueueCreate(MONIP_CMD_QUEUE_LEN, sizeof(monip_cmd_t));
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(MONIP_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "No MONIP config in NVS");
        return;
    }

    size_t len;

    len = sizeof(s_recv_a.ip);
    nvs_get_str(h, "ip_a", s_recv_a.ip, &len);

    nvs_get_u16(h, "port_a", &s_recv_a.port);

    len = sizeof(s_recv_b.ip);
    nvs_get_str(h, "ip_b", s_recv_b.ip, &len);

    nvs_get_u16(h, "port_b", &s_recv_b.port);

    len = sizeof(s_acct);
    nvs_get_str(h, "acct", s_acct, &len);

    len = sizeof(s_imei);
    nvs_get_str(h, "imei", s_imei, &len);

    uint8_t ct = 0;
    if (nvs_get_u8(h, "conn_type", &ct) == ESP_OK)
    {
        s_conn_type = (char)ct;
    }

    // Autonomous keepalive interval (written by CMD 0x10). 0 => default 60s.
    uint16_t ka = 0;
    if (nvs_get_u16(h, "keep_alive", &ka) == ESP_OK && ka > 0)
    {
        s_keepalive_sec = ka;
    }

    nvs_close(h);

    ESP_LOGI(TAG, "Config loaded: A=%s:%u B=%s:%u ACCT=%s IMEI=%s Type=%c KA=%us",
             s_recv_a.ip, s_recv_a.port, s_recv_b.ip, s_recv_b.port,
             s_acct, s_imei, s_conn_type, s_keepalive_sec);

    if (!s_cmd_task_started)
    {
        if (xTaskCreate(monip_cmd_task, "MONIP_cmd", MONIP_CMD_TASK_STACK, NULL, MONIP_CMD_TASK_PRIO, NULL) != pdPASS)
            ESP_LOGE(TAG, "Failed to create MONIP_cmd task - CID events cannot be sent");
        else
            s_cmd_task_started = true;
    }

    if (!s_rx_task_started)
    {
        if (xTaskCreate(monip_rx_task, "MONIP_rx", MONIP_RX_TASK_STACK, NULL, MONIP_RX_TASK_PRIO, NULL) != pdPASS)
            ESP_LOGE(TAG, "Failed to create MONIP_rx task - receiver ACK/NACK will not be processed");
        else
            s_rx_task_started = true;
    }

    if (!s_ka_task_started)
    {
        if (xTaskCreate(monip_keepalive_task, "MONIP_ka", MONIP_KA_TASK_STACK, NULL, MONIP_KA_TASK_PRIO, NULL) != pdPASS)
            ESP_LOGE(TAG, "Failed to create MONIP_ka task - keep-alive will not run");
        else
            s_ka_task_started = true;
    }
}

bool monip_submit_cmd(monip_cmd_t *cmd)
{
    if (s_cmd_queue == NULL)
    {
        ESP_LOGE(TAG, "MONIP command queue not initialized");
        if (cmd->payload)
            vPortFree(cmd->payload);
        return false;
    }

    if (xQueueSend(s_cmd_queue, cmd, 0) != pdTRUE)
    {
        ESP_LOGW(TAG, "MONIP command queue full, dropping command type=%d", cmd->type);
        if (cmd->payload)
            vPortFree(cmd->payload);
        return false;
    }

    return true;
}

bool monip_receiver_a_connected(void)
{
    return s_recv_a.connected;
}

bool monip_receiver_b_connected(void)
{
    return s_recv_b.connected;
}
