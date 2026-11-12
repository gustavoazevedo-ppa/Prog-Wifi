/*
 * Prog menu translator. See selector_tr_menu.h.
 *
 * States: MENU (from boot), CENTRAL (the central's screens pass through), STATUS
 * (PROGRAMMING... while the central is in master mode, planned).
 */

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_app_desc.h"
#include "device_info.h"
#include "selector_wire.h"
#include "wifi_manager.h"
#include "ethernet_manager.h"
#include "selector_keys.h"
#include "selector_tr_menu.h"

#define TAG "SEL_MENU"

#define CENTRAL_SILENCE_MS 3000 // without a valid central frame, the menu comes back
#define NET_POLL_MS 500         // how often the network state is re-read in the menu
#define TEXT_LEN 40             // scratch size for formatting before truncating to 16 columns

typedef enum
{
    MENU_STATE_MENU = 0,
    MENU_STATE_CENTRAL,
    MENU_STATE_STATUS
} menu_state_t;

static struct
{
    menu_state_t state;
    int page;
    uint32_t last_central_ms;
    uint32_t last_net_ms;
    uint8_t user_keys;     // keys currently pressed on the real selector
    uint8_t handback_mask; // keys held when the central took over: masked until released
    selector_keys_t keys;
    bool dirty;            // the screen must be emitted again
    char last_l1[SELECTOR_LCD_COLS + 1];
    char last_l2[SELECTOR_LCD_COLS + 1];
} s_menu = {.state = MENU_STATE_MENU, .dirty = true};

// ─── Output helpers ─────────────────────────────────────────────────────────

static void emit_screen(const char *l1, const char *l2)
{
    selector_msg_t m;
    memset(&m, 0, sizeof(m));
    m.type = SELECTOR_MSG_SCREEN;
    memset(m.u.screen.row, ' ', sizeof(m.u.screen.row));
    size_t n1 = strnlen(l1, SELECTOR_LCD_COLS);
    size_t n2 = strnlen(l2, SELECTOR_LCD_COLS);
    memcpy(m.u.screen.row[0], l1, n1);
    memcpy(m.u.screen.row[1], l2, n2);
    selector_bridge_emit(SELECTOR_DIR_DOWNSTREAM, &m);
}

static void emit_keys(uint8_t pressed)
{
    selector_msg_t m;
    memset(&m, 0, sizeof(m));
    m.type = SELECTOR_MSG_KEYS;
    m.u.keys.pressed = pressed;
    selector_bridge_emit(SELECTOR_DIR_UPSTREAM, &m);
}

// ─── Pages ──────────────────────────────────────────────────────────────────

static int page_count(void)
{
    return 1 + (wifi_is_enabled() ? 2 : 0) + (ethernet_is_enabled() ? 2 : 0);
}

static void copy_cols(char *dst, const char *src)
{
    memset(dst, 0, SELECTOR_LCD_COLS + 1);
    strncpy(dst, src, SELECTOR_LCD_COLS);
}

static void render_project(char *l1, char *l2)
{
    const esp_app_desc_t *app = esp_app_get_description();
    char name[TEXT_LEN];
    char ver[TEXT_LEN];
    char suffix[8];
    char line2[TEXT_LEN];

    // Uppercase, like every string the selector shows
    size_t i = 0;
    for (; i < sizeof(name) - 1 && app->project_name[i] != '\0'; i++)
    {
        name[i] = (char)toupper((unsigned char)app->project_name[i]);
    }
    name[i] = '\0';

    snprintf(suffix, sizeof(suffix), " HW%d", HW_VERSION);
    if (isdigit((unsigned char)app->version[0]))
    {
        snprintf(ver, sizeof(ver), "V%s", app->version);
    }
    else
    {
        snprintf(ver, sizeof(ver), "%s", app->version);
    }

    // Keep the hardware suffix when the version is too long
    size_t keep = SELECTOR_LCD_COLS - strlen(suffix);
    if (strlen(ver) > keep)
    {
        ver[keep] = '\0';
    }
    size_t ver_len = strlen(ver); // at most SELECTOR_LCD_COLS - strlen(suffix)
    memcpy(line2, ver, ver_len);
    memcpy(line2 + ver_len, suffix, strlen(suffix) + 1);

    copy_cols(l1, name);
    copy_cols(l2, line2);
}

static void format_ip(esp_netif_t *netif, char *out, size_t len)
{
    esp_netif_ip_info_t info;
    if (netif != NULL && esp_netif_get_ip_info(netif, &info) == ESP_OK)
    {
        snprintf(out, len, IPSTR, IP2STR(&info.ip));
    }
    else
    {
        snprintf(out, len, "0.0.0.0");
    }
}

// 17 characters with colons do not fit in 16 columns: 12 hex digits
static void format_mac(const uint8_t *mac, char *out, size_t len)
{
    snprintf(out, len, "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// kind 0 = IP, 1 = MAC. The MAC comes from the chip, so it is right even when the
// interface is down or its netif does not exist.
static void render_iface(bool wifi, int kind, char *l1, char *l2)
{
    char label[TEXT_LEN];
    char value[TEXT_LEN];
    uint8_t mac[6] = {0};

    snprintf(label, sizeof(label), "%s %s", wifi ? "WIFI" : "ETH", kind == 0 ? "IP" : "MAC");
    if (kind == 0)
    {
        format_ip(wifi ? wifi_get_netif() : ethernet_get_netif(), value, sizeof(value));
    }
    else
    {
        if (wifi)
        {
            esp_read_mac(mac, ESP_MAC_WIFI_STA);
        }
        else
        {
            ethernet_get_mac(mac);
        }
        format_mac(mac, value, sizeof(value));
    }
    copy_cols(l1, label);
    copy_cols(l2, value);
}

static void render_page(int page, char *l1, char *l2)
{
    if (page <= 0)
    {
        render_project(l1, l2);
        return;
    }

    int p = page - 1;
    if (wifi_is_enabled())
    {
        if (p < 2)
        {
            render_iface(true, p, l1, l2);
            return;
        }
        p -= 2;
    }
    if (ethernet_is_enabled() && p < 2)
    {
        render_iface(false, p, l1, l2);
        return;
    }
    render_project(l1, l2);
}

// Emits the current page when it changed (or when forced)
static void menu_show(bool force)
{
    char l1[SELECTOR_LCD_COLS + 1];
    char l2[SELECTOR_LCD_COLS + 1];

    render_page(s_menu.page, l1, l2);
    if (force || strcmp(l1, s_menu.last_l1) != 0 || strcmp(l2, s_menu.last_l2) != 0)
    {
        memcpy(s_menu.last_l1, l1, sizeof(l1));
        memcpy(s_menu.last_l2, l2, sizeof(l2));
        emit_screen(l1, l2);
    }
}

static void enter_menu(void)
{
    ESP_LOGI(TAG, "Central silent: showing the Prog menu");
    s_menu.state = MENU_STATE_MENU;
    s_menu.page = 0;
    s_menu.dirty = true;
    emit_keys(0); // the central's cached key state must be idle while it is silent
}

static void enter_central(void)
{
    ESP_LOGI(TAG, "Central talking: handing the screen back");
    s_menu.state = MENU_STATE_CENTRAL;
    s_menu.handback_mask = s_menu.user_keys; // keys held now stay released until let go
}

// ─── Translator callbacks ───────────────────────────────────────────────────

static void on_user_keys(uint8_t pressed)
{
    uint8_t edges = selector_keys_update(&s_menu.keys, pressed, NULL);
    s_menu.user_keys = pressed;

    switch (s_menu.state)
    {
    case MENU_STATE_MENU:
    {
        int count = page_count();
        if (edges & SELECTOR_KEY_SEL)
        {
            s_menu.page = (s_menu.page + 1) % count;
            menu_show(true);
        }
        else if (edges & SELECTOR_KEY_OK)
        {
            s_menu.page = (s_menu.page + count - 1) % count;
            menu_show(true);
        }
        break;
    }
    case MENU_STATE_CENTRAL:
        s_menu.handback_mask &= pressed;
        emit_keys((uint8_t)(pressed & ~s_menu.handback_mask));
        break;
    case MENU_STATE_STATUS:
    default:
        break;
    }
}

static void menu_on_msg(void *ctx, selector_dir_t dir, const selector_msg_t *msg)
{
    (void)ctx;

    if (dir == SELECTOR_DIR_UPSTREAM)
    {
        if (msg->type == SELECTOR_MSG_KEYS)
        {
            on_user_keys(msg->u.keys.pressed);
        }
        return;
    }

    // Any valid frame from the central takes the screen back at once
    s_menu.last_central_ms = selector_bridge_now_ms();
    if (s_menu.state != MENU_STATE_CENTRAL)
    {
        enter_central();
    }
    selector_bridge_emit(SELECTOR_DIR_DOWNSTREAM, msg);
}

static void menu_on_link(void *ctx, selector_side_t side, bool up)
{
    (void)ctx;
    // The selector may have been reset or replugged: draw the menu again
    if (side == SELECTOR_SIDE_SELECTOR && up)
    {
        s_menu.dirty = true;
    }
}

static void menu_on_mode(void *ctx, bool central_slave)
{
    (void)ctx;
    if (central_slave)
    {
        s_menu.state = MENU_STATE_STATUS;
        emit_screen("PROGRAMMING...", "");
        emit_keys(0);
    }
    else if (s_menu.state == MENU_STATE_STATUS)
    {
        // Back to the central: the 3 s rule applies if it does not talk again
        s_menu.state = MENU_STATE_CENTRAL;
        s_menu.last_central_ms = selector_bridge_now_ms();
        s_menu.handback_mask = s_menu.user_keys;
    }
}

static void menu_on_tick(void *ctx, uint32_t now_ms)
{
    (void)ctx;

    if (s_menu.state == MENU_STATE_CENTRAL &&
        (uint32_t)(now_ms - s_menu.last_central_ms) >= CENTRAL_SILENCE_MS)
    {
        enter_menu();
    }

    if (s_menu.state == MENU_STATE_MENU)
    {
        if (s_menu.dirty)
        {
            s_menu.dirty = false;
            s_menu.last_net_ms = now_ms;
            menu_show(true);
        }
        else if ((uint32_t)(now_ms - s_menu.last_net_ms) >= NET_POLL_MS)
        {
            s_menu.last_net_ms = now_ms;
            menu_show(false);
        }
    }
}

static const selector_translator_t s_menu_translator = {
    .ctx = NULL,
    .on_msg = menu_on_msg,
    .on_link = menu_on_link,
    .on_mode = menu_on_mode,
    .on_tick = menu_on_tick,
};

const selector_translator_t *selector_tr_menu(void)
{
    return &s_menu_translator;
}
