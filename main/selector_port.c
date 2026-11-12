/*
 * Selector ports. See selector_port.h.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "selector_port.h"

#define PORT_RX_BUF 256
#define PORT_TX_BUF 256
#define PORT_TASK_STACK 4096
#define PORT_TASK_PRIO 5
#define PORT_READ_TIMEOUT_MS 10 // at least one tick: also bounds the supervision latency
#define DROP_LOG_PERIOD_MS 1000

struct selector_port
{
    selector_port_cfg_t cfg;
    const char *tag;
    selector_framer_t framer;
    selector_port_stats_t stats;
    uint32_t decode_drops[SELECTOR_DROP_REASON_COUNT];
    uint32_t drops_logged;
    uint32_t last_drop_log_ms;
    bool link_up;
    uint32_t last_valid_ms;

    // Selector role (faces the central)
    uint8_t cached_keys;
    selector_screen_t last_screen;

    // Host role (faces the real selector)
    selector_screen_t screen;
    bool screen_valid;
    uint8_t backlight;
    bool backlight_set;
    bool backlight_dirty;
    selector_msg_t beep;
    bool beep_pending;
    uint8_t last_keys;
    bool keys_known;
};

static volatile bool s_tap = false;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

void selector_port_set_tap(bool on)
{
    s_tap = on;
}

static void port_tap(const selector_port_t *p, const char *dir, const uint8_t *buf, size_t len)
{
    if (!s_tap)
    {
        return;
    }
    char line[SELECTOR_MAX_FRAME * 3 + 1];
    size_t pos = 0;
    for (size_t i = 0; i < len && pos + 4 < sizeof(line); i++)
    {
        pos += (size_t)snprintf(&line[pos], sizeof(line) - pos, "%02X ", buf[i]);
    }
    line[pos] = '\0';
    ESP_LOGI(p->tag, "%s %s", dir, line);
}

static esp_err_t port_uart_init(const selector_port_cfg_t *cfg)
{
    const uart_config_t uc = {
        .baud_rate = (int)cfg->baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_driver_install(cfg->uart, PORT_RX_BUF, PORT_TX_BUF, 0, NULL, 0);
    if (err != ESP_OK)
    {
        return err;
    }
    err = uart_param_config(cfg->uart, &uc);
    if (err == ESP_OK)
    {
        err = uart_set_pin(cfg->uart, cfg->tx_pin, cfg->rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (err != ESP_OK)
    {
        uart_driver_delete(cfg->uart);
    }
    return err;
}

static void port_send_evt(selector_port_t *p, const selector_evt_t *evt)
{
    if (xQueueSend(p->cfg.to_bridge, evt, 0) != pdTRUE)
    {
        p->stats.queue_full++;
    }
}

static selector_side_t port_side(const selector_port_t *p)
{
    return p->cfg.role == SELECTOR_ROLE_SELECTOR ? SELECTOR_SIDE_CENTRAL : SELECTOR_SIDE_SELECTOR;
}

static void port_set_link(selector_port_t *p, bool up)
{
    if (p->link_up == up)
    {
        return;
    }
    p->link_up = up;
    p->stats.link_up = up;
    p->stats.link_changes++;

    selector_evt_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.type = SELECTOR_EVT_LINK;
    evt.side = port_side(p);
    evt.up = up;
    port_send_evt(p, &evt);
    ESP_LOGI(p->tag, "Link %s", up ? "UP" : "DOWN");
}

static uint32_t port_drop_total(const selector_port_t *p)
{
    uint32_t total = 0;
    for (int i = 1; i < SELECTOR_DROP_REASON_COUNT; i++)
    {
        total += p->framer.drops[i] + p->decode_drops[i];
    }
    return total;
}

// Logs dropped frames at most once per DROP_LOG_PERIOD_MS
static void port_check_drops(selector_port_t *p, uint32_t now)
{
    uint32_t total = port_drop_total(p);
    if (total == p->drops_logged || (uint32_t)(now - p->last_drop_log_ms) < DROP_LOG_PERIOD_MS)
    {
        return;
    }
    p->drops_logged = total;
    p->last_drop_log_ms = now;
    ESP_LOGW(p->tag, "Dropped frames (total %u): size=%u stop=%u cks=%u id=%u len=%u func=%u partial=%u",
             (unsigned)total,
             (unsigned)(p->framer.drops[SELECTOR_DROP_BAD_SIZE] + p->decode_drops[SELECTOR_DROP_BAD_SIZE]),
             (unsigned)(p->framer.drops[SELECTOR_DROP_BAD_STOP] + p->decode_drops[SELECTOR_DROP_BAD_STOP]),
             (unsigned)(p->framer.drops[SELECTOR_DROP_BAD_CHECKSUM] + p->decode_drops[SELECTOR_DROP_BAD_CHECKSUM]),
             (unsigned)(p->framer.drops[SELECTOR_DROP_BAD_ID] + p->decode_drops[SELECTOR_DROP_BAD_ID]),
             (unsigned)(p->framer.drops[SELECTOR_DROP_BAD_LENGTH] + p->decode_drops[SELECTOR_DROP_BAD_LENGTH]),
             (unsigned)(p->framer.drops[SELECTOR_DROP_UNKNOWN_FUNCTION] + p->decode_drops[SELECTOR_DROP_UNKNOWN_FUNCTION]),
             (unsigned)(p->framer.drops[SELECTOR_DROP_PARTIAL_TIMEOUT] + p->decode_drops[SELECTOR_DROP_PARTIAL_TIMEOUT]));
}

void selector_port_get_stats(const selector_port_t *p, selector_port_stats_t *out)
{
    *out = p->stats;
    for (int i = 0; i < SELECTOR_DROP_REASON_COUNT; i++)
    {
        out->drops[i] = p->framer.drops[i] + p->decode_drops[i];
    }
}

// ─── Selector role (faces the central) ──────────────────────────────────────

// Upstream key state from the bridge only updates the cache: nothing is sent unsolicited
static void panel_drain(selector_port_t *p)
{
    selector_msg_t m;
    while (xQueueReceive(p->cfg.from_bridge, &m, 0) == pdTRUE)
    {
        if (m.type == SELECTOR_MSG_KEYS)
        {
            p->cached_keys = m.u.keys.pressed;
        }
    }
}

static void panel_handle_frame(selector_port_t *p, const selector_frame_t *frame, uint32_t now)
{
    selector_msg_t msg;
    selector_drop_reason_t reason;

    if (selector_decode(frame, SELECTOR_DIR_DOWNSTREAM, &p->last_screen, &msg, &reason) != SELECTOR_DECODE_MSG)
    {
        p->decode_drops[reason]++;
        return;
    }

    // Reply at once, from the cached keys, whatever the bridge does with the message
    panel_drain(p);
    uint8_t reply[SELECTOR_REPLY_FRAME];
    size_t len = selector_encode_reply(frame->bytes[3], p->cached_keys, reply);
    uart_write_bytes(p->cfg.uart, (const char *)reply, len);
    port_tap(p, "RX", frame->bytes, frame->len);
    port_tap(p, "TX", reply, len);

    p->stats.frames_ok++;
    p->last_valid_ms = now;
    port_set_link(p, true);

    if (msg.type == SELECTOR_MSG_SCREEN)
    {
        p->last_screen = msg.u.screen;
    }

    selector_evt_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.type = SELECTOR_EVT_MSG;
    evt.side = SELECTOR_SIDE_CENTRAL;
    evt.dir = SELECTOR_DIR_DOWNSTREAM;
    evt.msg = msg;
    port_send_evt(p, &evt);
}

static void panel_task(void *arg)
{
    selector_port_t *p = arg;
    selector_frame_t frame;
    uint8_t byte;

    while (1)
    {
        int n = uart_read_bytes(p->cfg.uart, &byte, 1, pdMS_TO_TICKS(PORT_READ_TIMEOUT_MS));
        uint32_t now = now_ms();

        if (n == 1)
        {
            bool got = selector_framer_feed(&p->framer, byte, now, &frame);
            while (got)
            {
                panel_handle_frame(p, &frame, now);
                got = selector_framer_next(&p->framer, &frame);
            }
        }
        else
        {
            panel_drain(p);
        }

        port_check_drops(p, now);
        if (p->link_up && (uint32_t)(now - p->last_valid_ms) > p->cfg.link_timeout_ms)
        {
            port_set_link(p, false);
        }
    }
}

// ─── Host role (faces the real selector) ────────────────────────────────────

static void host_drain(selector_port_t *p)
{
    selector_msg_t m;
    while (xQueueReceive(p->cfg.from_bridge, &m, 0) == pdTRUE)
    {
        switch (m.type)
        {
        case SELECTOR_MSG_SCREEN:
            p->screen = m.u.screen;
            p->screen_valid = true;
            break;
        case SELECTOR_MSG_BEEP:
            p->beep = m;
            p->beep_pending = true;
            break;
        case SELECTOR_MSG_BACKLIGHT:
            p->backlight = m.u.backlight.level;
            p->backlight_set = true;
            p->backlight_dirty = true;
            break;
        default:
            break;
        }
    }
}

// One frame per cycle, in priority order: beep, backlight, screen. While the selector is
// not answering (or there is no screen yet) only the key poll is sent.
static void host_send_next(selector_port_t *p)
{
    selector_msg_t m;
    uint8_t out[SELECTOR_MAX_FRAME];

    memset(&m, 0, sizeof(m));
    if (!p->link_up || !p->screen_valid)
    {
        m.type = SELECTOR_MSG_POLL;
    }
    else if (p->beep_pending)
    {
        m = p->beep;
        p->beep_pending = false;
    }
    else if (p->backlight_dirty)
    {
        m.type = SELECTOR_MSG_BACKLIGHT;
        m.u.backlight.level = p->backlight;
        p->backlight_dirty = false;
    }
    else
    {
        m.type = SELECTOR_MSG_SCREEN;
        m.u.screen = p->screen;
    }

    size_t len = selector_encode(&m, out);
    if (len > 0)
    {
        uart_write_bytes(p->cfg.uart, (const char *)out, len);
        port_tap(p, "TX", out, len);
    }
}

static void host_handle_frame(selector_port_t *p, const selector_frame_t *frame, uint32_t now, bool *awaiting)
{
    selector_msg_t msg;
    selector_drop_reason_t reason;
    selector_decode_result_t r = selector_decode(frame, SELECTOR_DIR_UPSTREAM, NULL, &msg, &reason);

    if (r == SELECTOR_DECODE_DROP)
    {
        p->decode_drops[reason]++;
        return;
    }

    port_tap(p, "RX", frame->bytes, frame->len);
    p->stats.frames_ok++;
    p->last_valid_ms = now;
    *awaiting = false;

    if (!p->link_up)
    {
        port_set_link(p, true);
        // The selector may have been reset or replugged: send the backlight again (the screen
        // is resent on every cycle anyway) and report the key state once
        p->backlight_dirty = p->backlight_set;
        p->keys_known = false;
    }

    if (r == SELECTOR_DECODE_MSG && (!p->keys_known || msg.u.keys.pressed != p->last_keys))
    {
        p->keys_known = true;
        p->last_keys = msg.u.keys.pressed;

        selector_evt_t evt;
        memset(&evt, 0, sizeof(evt));
        evt.type = SELECTOR_EVT_MSG;
        evt.side = SELECTOR_SIDE_SELECTOR;
        evt.dir = SELECTOR_DIR_UPSTREAM;
        evt.msg = msg;
        port_send_evt(p, &evt);
    }
}

static void host_task(void *arg)
{
    selector_port_t *p = arg;
    selector_frame_t frame;
    uint8_t byte;
    bool awaiting = false;
    uint32_t deadline = 0;
    uint32_t next_cycle = now_ms();

    while (1)
    {
        host_drain(p);

        uint32_t now = now_ms();
        if ((int32_t)(now - next_cycle) >= 0)
        {
            next_cycle = now + p->cfg.refresh_ms;
            host_send_next(p);
            awaiting = true;
            deadline = now + p->cfg.reply_timeout_ms;
        }

        int n = uart_read_bytes(p->cfg.uart, &byte, 1, pdMS_TO_TICKS(PORT_READ_TIMEOUT_MS));
        now = now_ms();
        if (n == 1)
        {
            bool got = selector_framer_feed(&p->framer, byte, now, &frame);
            while (got)
            {
                host_handle_frame(p, &frame, now, &awaiting);
                got = selector_framer_next(&p->framer, &frame);
            }
        }

        if (awaiting && (int32_t)(now - deadline) >= 0)
        {
            awaiting = false;
            p->stats.reply_timeouts++;
        }

        port_check_drops(p, now);
        if (p->link_up && (uint32_t)(now - p->last_valid_ms) > p->cfg.link_timeout_ms)
        {
            port_set_link(p, false);
        }
    }
}

selector_port_t *selector_port_start(const selector_port_cfg_t *cfg)
{
    selector_port_t *p = calloc(1, sizeof(*p));
    if (p == NULL)
    {
        return NULL;
    }
    p->cfg = *cfg;
    p->tag = cfg->role == SELECTOR_ROLE_SELECTOR ? "SEL_PANEL" : "SEL_HOST";
    selector_framer_init(&p->framer, SELECTOR_ID, cfg->partial_timeout_ms);
    memset(p->last_screen.row, ' ', sizeof(p->last_screen.row));
    p->cached_keys = 0;

    esp_err_t err = port_uart_init(cfg);
    if (err != ESP_OK)
    {
        ESP_LOGE(p->tag, "UART%d init failed: %s", (int)cfg->uart, esp_err_to_name(err));
        free(p);
        return NULL;
    }

    bool selector_role = cfg->role == SELECTOR_ROLE_SELECTOR;
    if (xTaskCreate(selector_role ? panel_task : host_task,
                    selector_role ? "sel_panel" : "sel_host",
                    PORT_TASK_STACK, p, PORT_TASK_PRIO, NULL) != pdPASS)
    {
        ESP_LOGE(p->tag, "Task create failed");
        uart_driver_delete(cfg->uart);
        free(p);
        return NULL;
    }

    ESP_LOGI(p->tag, "Started on UART%d (TX GPIO%d, RX GPIO%d, %u baud)",
             (int)cfg->uart, cfg->tx_pin, cfg->rx_pin, (unsigned)cfg->baud);
    return p;
}
