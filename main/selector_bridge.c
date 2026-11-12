/*
 * Selector bridge: queues, ports and the translator task. See selector_bridge.h.
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "selector_bridge.h"

#define TAG "SELECTOR"

// UART2 faces the central (DUT), UART1 faces the real selector
#define CENTRAL_UART UART_NUM_2
#define CENTRAL_TX_PIN 17
#define CENTRAL_RX_PIN 16
#define SELECTOR_UART UART_NUM_1
#define SELECTOR_TX_PIN 25
#define SELECTOR_RX_PIN 26
#define SELECTOR_BAUD 9600

#define PARTIAL_TIMEOUT_MS 25
#define LINK_TIMEOUT_MS 500
#define REFRESH_MS 100
#define REPLY_TIMEOUT_MS 80

#define BRIDGE_TASK_STACK 4096
#define BRIDGE_TASK_PRIO 4
#define BRIDGE_IN_DEPTH 8
#define PORT_IN_DEPTH 8

static QueueHandle_t s_bridge_in;   // selector_evt_t, from both ports
static QueueHandle_t s_panel_in;    // selector_msg_t, to the port facing the central
static QueueHandle_t s_selector_in; // selector_msg_t, to the port facing the real selector
static selector_port_t *s_panel;
static selector_port_t *s_selector;
static const selector_translator_t *s_tr;
static uint32_t s_queue_full;

uint32_t selector_bridge_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

void selector_bridge_emit(selector_dir_t dir, const selector_msg_t *msg)
{
    QueueHandle_t q;

    if (dir == SELECTOR_DIR_DOWNSTREAM)
    {
        if (msg->type != SELECTOR_MSG_SCREEN && msg->type != SELECTOR_MSG_BEEP &&
            msg->type != SELECTOR_MSG_BACKLIGHT)
        {
            return;
        }
        q = s_selector_in;
    }
    else
    {
        if (msg->type != SELECTOR_MSG_KEYS)
        {
            return;
        }
        q = s_panel_in;
    }

    if (q == NULL || xQueueSend(q, msg, 0) != pdTRUE)
    {
        s_queue_full++;
    }
}

// ─── Identity translator ────────────────────────────────────────────────────

static void identity_on_msg(void *ctx, selector_dir_t dir, const selector_msg_t *msg)
{
    (void)ctx;
    selector_bridge_emit(dir, msg);
}

static const selector_translator_t s_identity = {
    .ctx = NULL,
    .on_msg = identity_on_msg,
    .on_link = NULL,
    .on_mode = NULL,
    .on_tick = NULL,
};

const selector_translator_t *selector_tr_identity(void)
{
    return &s_identity;
}

// ─── Bridge task ────────────────────────────────────────────────────────────

static void bridge_dispatch(const selector_evt_t *evt)
{
    switch (evt->type)
    {
    case SELECTOR_EVT_MSG:
        if (s_tr->on_msg != NULL)
        {
            s_tr->on_msg(s_tr->ctx, evt->dir, &evt->msg);
        }
        break;
    case SELECTOR_EVT_LINK:
        if (s_tr->on_link != NULL)
        {
            s_tr->on_link(s_tr->ctx, evt->side, evt->up);
        }
        break;
    case SELECTOR_EVT_MODE:
        if (s_tr->on_mode != NULL)
        {
            s_tr->on_mode(s_tr->ctx, evt->up);
        }
        break;
    default:
        break;
    }
}

static void bridge_task(void *arg)
{
    selector_evt_t evt;
    uint32_t last_tick = selector_bridge_now_ms();

    if (s_tr->on_tick != NULL)
    {
        s_tr->on_tick(s_tr->ctx, last_tick); // lets the translator draw its first screen
    }

    while (1)
    {
        if (xQueueReceive(s_bridge_in, &evt, pdMS_TO_TICKS(SELECTOR_BRIDGE_TICK_MS)) == pdTRUE)
        {
            bridge_dispatch(&evt);
        }

        uint32_t now = selector_bridge_now_ms();
        if ((uint32_t)(now - last_tick) >= SELECTOR_BRIDGE_TICK_MS)
        {
            last_tick = now;
            if (s_tr->on_tick != NULL)
            {
                s_tr->on_tick(s_tr->ctx, now);
            }
        }
    }
}

bool selector_bridge_start(const selector_translator_t *translator)
{
    if (!selector_selftest())
    {
        ESP_LOGE(TAG, "Wire self-test failed");
        return false;
    }

    s_tr = translator != NULL ? translator : &s_identity;
    s_bridge_in = xQueueCreate(BRIDGE_IN_DEPTH, sizeof(selector_evt_t));
    s_panel_in = xQueueCreate(PORT_IN_DEPTH, sizeof(selector_msg_t));
    s_selector_in = xQueueCreate(PORT_IN_DEPTH, sizeof(selector_msg_t));
    if (s_bridge_in == NULL || s_panel_in == NULL || s_selector_in == NULL)
    {
        ESP_LOGE(TAG, "Queue create failed");
        return false;
    }

    const selector_port_cfg_t panel_cfg = {
        .role = SELECTOR_ROLE_SELECTOR,
        .uart = CENTRAL_UART,
        .tx_pin = CENTRAL_TX_PIN,
        .rx_pin = CENTRAL_RX_PIN,
        .baud = SELECTOR_BAUD,
        .partial_timeout_ms = PARTIAL_TIMEOUT_MS,
        .link_timeout_ms = LINK_TIMEOUT_MS,
        .refresh_ms = 0,
        .reply_timeout_ms = 0,
        .to_bridge = s_bridge_in,
        .from_bridge = s_panel_in,
    };
    const selector_port_cfg_t selector_cfg = {
        .role = SELECTOR_ROLE_HOST,
        .uart = SELECTOR_UART,
        .tx_pin = SELECTOR_TX_PIN,
        .rx_pin = SELECTOR_RX_PIN,
        .baud = SELECTOR_BAUD,
        .partial_timeout_ms = PARTIAL_TIMEOUT_MS,
        .link_timeout_ms = LINK_TIMEOUT_MS,
        .refresh_ms = REFRESH_MS,
        .reply_timeout_ms = REPLY_TIMEOUT_MS,
        .to_bridge = s_bridge_in,
        .from_bridge = s_selector_in,
    };

    s_panel = selector_port_start(&panel_cfg);
    s_selector = selector_port_start(&selector_cfg);
    if (s_panel == NULL || s_selector == NULL)
    {
        ESP_LOGE(TAG, "Port start failed");
        return false;
    }

    if (xTaskCreate(bridge_task, "sel_bridge", BRIDGE_TASK_STACK, NULL, BRIDGE_TASK_PRIO, NULL) != pdPASS)
    {
        ESP_LOGE(TAG, "Bridge task create failed");
        return false;
    }

    ESP_LOGI(TAG, "Bridge started");
    return true;
}

void selector_bridge_get_stats(selector_bridge_stats_t *out)
{
    memset(out, 0, sizeof(*out));
    if (s_panel != NULL)
    {
        selector_port_get_stats(s_panel, &out->panel);
    }
    if (s_selector != NULL)
    {
        selector_port_get_stats(s_selector, &out->selector);
    }
    out->bridge_queue_full = s_queue_full;
}

void selector_bridge_set_tap(bool on)
{
    selector_port_set_tap(on);
}
