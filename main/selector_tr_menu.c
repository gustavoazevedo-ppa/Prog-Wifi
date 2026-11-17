/*
 * Prog menu translator. See selector_tr_menu.h.
 *
 * States: MENU (from boot, and after every exit), CENTRAL (the central's screens and the keys pass
 * through, also while a macro is recorded), REPLAY (state only for now, no engine: milestone C), STATUS (PROGRAMMING... while the
 * central is in master mode, planned).
 *
 * The menu does not hand the screen to the central by itself. While it shows, the panel port's reply
 * gate is closed, so the central keeps sending its start screen (the pop-up), which is stored here.
 * [ENTER CENTRAL] opens the gate. Every arrival in MENU runs the reset procedure: no replies until
 * the central shows its stored pop-up again, POPUP_RESET_WAIT_MS at most.
 * Design: docs/selector-bridge-design.md section 7c.
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
#include "preset_server.h"
#include "selector_keys.h"
#include "selector_macro.h"
#include "selector_tr_menu.h"

#define TAG "SEL_MENU"

#define CENTRAL_SILENCE_MS 3000  // without a valid central frame, the menu comes back
#define CENTRAL_KEY_IDLE_MS 3000 // with no key pressed on the real selector (CENTRAL only); a held key restarts it, same
#define POPUP_RESET_WAIT_MS 5000 // reset procedure: how long to wait for the central's start screen
#define NET_POLL_MS 500          // how often the network state is re-read in the menu
#define ENTRY_POLL_MS 100        // how often the availability reason of a macro entry is re-read
#define TEXT_LEN 40              // scratch size for formatting before truncating to 16 columns
#define STORE_RETRY_TICKS 100    // a recorded macro that could not be stored is retried this many ticks (about 1 s)

// Menu pages: the project page, the three entries, then the IP and MAC pages
#define PAGE_PROJECT 0
#define PAGE_ENTER 1
#define PAGE_RECORD 2
#define PAGE_REPLAY 3
#define PAGE_IFACE_FIRST 4

typedef enum
{
    MENU_STATE_MENU = 0,
    MENU_STATE_CENTRAL,
    MENU_STATE_REPLAY,
    MENU_STATE_STATUS
} menu_state_t;

// Which central screen is taken as the pop-up while the gate is closed and none is stored.
// Confirmed on the bench (open questions F2).
typedef enum
{
    CAPTURE_NONE = 0,
    CAPTURE_FIRST, // the first screen after the central goes from absent to present (incl. boot)
    CAPTURE_DIFF   // the first screen that differs from the last one before the gate closed
} capture_t;

static struct
{
    menu_state_t state;
    bool started; // the first tick has run (gate closed, reset procedure begun)
    int page;
    bool central_present;     // a valid central frame arrived within CENTRAL_SILENCE_MS (any state)
    uint32_t last_central_ms; // last valid central frame, drives presence and the exit rules
    uint32_t last_key_ms; // last time a key was pressed or changed on the real selector (a held key keeps it current), for the key idle exit
    uint32_t last_net_ms;
    uint8_t user_keys;     // keys currently pressed on the real selector
    uint8_t handback_mask; // keys held when the central took over: masked until released
    selector_keys_t keys;
    bool dirty;            // the screen must be emitted again
    char last_l1[SELECTOR_LCD_COLS + 1];
    char last_l2[SELECTOR_LCD_COLS + 1];

    // Pop-up store and reset procedure
    char popup[SELECTOR_MACRO_POPUP_MAX]; // the central's start screen, normalized
    bool popup_valid;
    bool popup_ready;      // the central is showing its start screen again (its menus are reset)
    bool reset_active;     // the reset wait is running
    uint32_t reset_start_ms;
    char last_norm[SELECTOR_MACRO_POPUP_MAX]; // the last central screen, normalized
    char ref_norm[SELECTOR_MACRO_POPUP_MAX];  // the last central screen before the gate closed
    capture_t capture;

    // Macro recording and the cached answer of the scratchpad query
    bool recording;
    bool store_pending; // the recorded macro is in s_rec_buf and still has to be stored
    int store_tries;
    bool macro_cached; // last good answer of preset_server_macro_query()
    bool claim_held;   // the selector claim of preset_server is held (see claim_service())
} s_menu = {.state = MENU_STATE_MENU, .dirty = true, .capture = CAPTURE_NONE};

static uint8_t s_rec_buf[SELECTOR_MACRO_MAX];
static size_t s_rec_len;
static selector_macro_enc_t s_enc;

// Mirror of "state != MENU", written only through menu_set_state() in the bridge task and read by
// the HTTP task (POST /iniciar is refused while it is true). A single byte, so no lock is needed.
static volatile bool s_transparent;

static void menu_set_state(menu_state_t st)
{
    s_menu.state = st;
    s_transparent = (st != MENU_STATE_MENU);
}

// The claim stays held while the menu is transparent and while a recorded macro waits to be stored,
// so /iniciar cannot start a run that the late store would overwrite. The release is bounded (5 ms)
// and retried on later ticks; the claim is kept until it succeeds.
static void claim_service(void)
{
    if (s_menu.claim_held && s_menu.state == MENU_STATE_MENU && !s_menu.store_pending)
    {
        if (preset_server_selector_release())
        {
            s_menu.claim_held = false; // /iniciar is accepted again
        }
    }
}

bool selector_tr_menu_transparent(void)
{
    return s_transparent;
}

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
    return PAGE_IFACE_FIRST + (wifi_is_enabled() ? 2 : 0) + (ethernet_is_enabled() ? 2 : 0);
}

// A valid macro is in the scratchpad. The query never blocks for long; on a lock timeout the last
// good answer is kept and asked again on the next poll.
static bool macro_in_scratchpad(void)
{
    bool present;
    if (preset_server_macro_query(&present))
    {
        s_menu.macro_cached = present;
    }
    return s_menu.macro_cached;
}

// Why a macro entry cannot be used now, or NULL when it can. First match wins.
static const char *entry_reason(int page)
{
    if (page == PAGE_ENTER)
    {
        return NULL;
    }
    if (preset_server_run_active() || (page == PAGE_RECORD && s_menu.store_pending))
    {
        return "BUSY";
    }
    if (page == PAGE_REPLAY && !macro_in_scratchpad())
    {
        return "NO MACRO";
    }
    if (!s_menu.popup_valid)
    {
        return "NO CENTRAL";
    }
    if (!s_menu.popup_ready)
    {
        return "WAIT CENTRAL";
    }
    return NULL;
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
    if (page == PAGE_PROJECT)
    {
        render_project(l1, l2);
        return;
    }
    if (page >= PAGE_ENTER && page < PAGE_IFACE_FIRST)
    {
        const char *reason = entry_reason(page);
        copy_cols(l1, page == PAGE_ENTER ? "[ENTER CENTRAL]" : page == PAGE_RECORD ? "[RECORD MACRO]" : "[REPLAY MACRO]");
        copy_cols(l2, reason != NULL ? reason : "PLUS = GO");
        return;
    }

    int p = page - PAGE_IFACE_FIRST;
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

// ─── Pop-up store and reset procedure ───────────────────────────────────────

static void popup_clear(void)
{
    s_menu.popup[0] = '\0';
    s_menu.popup_valid = false;
    s_menu.popup_ready = false;
}

// Called for every screen the central sends, in every state
static void popup_track(const selector_screen_t *screen)
{
    char norm[SELECTOR_MACRO_POPUP_MAX];
    size_t n = selector_macro_normalize_popup(screen->row, norm, sizeof(norm));

    // Only while the gate is closed: the central then shows its start screen
    if (s_menu.state == MENU_STATE_MENU && n > 0)
    {
        if (s_menu.popup_valid)
        {
            if (strcmp(norm, s_menu.popup) == 0 && !s_menu.popup_ready)
            {
                s_menu.popup_ready = true;
                s_menu.reset_active = false;
                ESP_LOGI(TAG, "Central is back on its start screen");
            }
        }
        else if (s_menu.capture == CAPTURE_FIRST ||
                 (s_menu.capture == CAPTURE_DIFF && strcmp(norm, s_menu.ref_norm) != 0))
        {
            memcpy(s_menu.popup, norm, sizeof(s_menu.popup));
            s_menu.popup_valid = true;
            s_menu.popup_ready = true; // a start screen that was just captured is by definition reset
            s_menu.reset_active = false;
            s_menu.capture = CAPTURE_NONE;
            ESP_LOGI(TAG, "Pop-up stored: %s", s_menu.popup);
        }
    }
    memcpy(s_menu.last_norm, norm, sizeof(s_menu.last_norm));
}

// Bench (open questions F1): the bench central shows its start screen again after about 1 s without
// communication, other models certainly within 5 s, so POPUP_RESET_WAIT_MS stays.
static void reset_begin(uint32_t now_ms)
{
    s_menu.popup_ready = false;
    s_menu.reset_active = true;
    s_menu.reset_start_ms = now_ms;
}

// ─── State changes ──────────────────────────────────────────────────────────

// Stores the recorded macro. Invalid content is dropped; a lock timeout keeps it pending.
static void store_try(void)
{
    if (selector_macro_validate(s_rec_buf, s_rec_len) != SELECTOR_MACRO_OK)
    {
        ESP_LOGW(TAG, "Recorded macro is not valid (%u bytes): dropped", (unsigned)s_rec_len);
        s_menu.store_pending = false;
    }
    else if (preset_server_macro_store(s_rec_buf, s_rec_len))
    {
        ESP_LOGI(TAG, "Macro stored in the scratchpad (%u bytes)", (unsigned)s_rec_len);
        s_menu.store_pending = false;
        s_menu.macro_cached = true;
    }
    else if (++s_menu.store_tries >= STORE_RETRY_TICKS)
    {
        ESP_LOGE(TAG, "recorded macro could not be stored");
        s_menu.store_pending = false;
    }
}

static void recording_finish(void)
{
    s_menu.recording = false;
    s_rec_len = selector_macro_enc_finish(&s_enc);
    s_menu.store_pending = true;
    s_menu.store_tries = 0;
    store_try();
}

static void enter_menu(uint32_t now_ms)
{
    ESP_LOGI(TAG, "Showing the Prog menu");
    if (s_menu.recording)
    {
        recording_finish();
    }
    menu_set_state(MENU_STATE_MENU);
    s_menu.page = 0;
    s_menu.dirty = true;
    selector_bridge_set_reply_gate(false);
    emit_keys(0); // the central's cached key state must be idle while it is silent

    // Reset procedure: the central must show its start screen again (rule on capture: see
    // capture_t, unverified)
    memcpy(s_menu.ref_norm, s_menu.last_norm, sizeof(s_menu.ref_norm));
    s_menu.capture = CAPTURE_DIFF;
    reset_begin(now_ms);
}

static void enter_central(uint32_t now_ms)
{
    ESP_LOGI(TAG, "Central transparent: gate open");
    menu_set_state(MENU_STATE_CENTRAL);
    s_menu.handback_mask = s_menu.user_keys; // keys held now stay released until let go
    s_menu.last_central_ms = now_ms;
    s_menu.last_key_ms = now_ms;
    s_menu.reset_active = false; // the next arrival in MENU starts a new reset
    selector_bridge_set_reply_gate(true);
}

// PLUS on a menu entry
static void menu_pick(uint32_t now_ms)
{
    switch (s_menu.page)
    {
    case PAGE_ENTER:
        // The claim is atomic with the start of a run: it fails while a run is active
        if (s_menu.claim_held || preset_server_selector_claim())
        {
            s_menu.claim_held = true;
            enter_central(now_ms);
        }
        break;
    case PAGE_RECORD:
        if (entry_reason(PAGE_RECORD) == NULL)
        {
            if (!selector_macro_enc_begin(&s_enc, s_rec_buf, sizeof(s_rec_buf), s_menu.popup))
            {
                ESP_LOGW(TAG, "Recording not started: the header does not fit");
                break;
            }
            if (!s_menu.claim_held && !preset_server_selector_claim())
            {
                break; // a run started meanwhile: stay in the menu
            }
            s_menu.claim_held = true;
            s_menu.recording = true;
            ESP_LOGI(TAG, "Recording started");
            enter_central(now_ms);
        }
        break;
    case PAGE_REPLAY:
        if (entry_reason(PAGE_REPLAY) == NULL)
        {
            ESP_LOGI(TAG, "Macro replay picked: replay not implemented yet (milestone C)");
        }
        break;
    default:
        break;
    }
}

// ─── Translator callbacks ───────────────────────────────────────────────────

static void on_user_keys(uint8_t pressed)
{
    uint32_t now = selector_bridge_now_ms();
    bool changed = pressed != s_menu.user_keys;
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
        else if (edges & SELECTOR_KEY_PLUS)
        {
            menu_pick(now);
        }
        break;
    }
    case MENU_STATE_CENTRAL:
        if (changed)
        {
            s_menu.last_key_ms = now;
        }
        s_menu.handback_mask &= pressed;
        emit_keys((uint8_t)(pressed & ~s_menu.handback_mask));
        break;
    case MENU_STATE_REPLAY: // the real selector's keys are ignored during a replay
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

    uint32_t now = selector_bridge_now_ms();
    s_menu.last_central_ms = now;
    if (!s_menu.central_present)
    {
        // Absent to present (also at boot): the first screen from now on is the pop-up
        s_menu.central_present = true;
        s_menu.capture = CAPTURE_FIRST;
        ESP_LOGI(TAG, "Central present");
    }
    if (msg->type == SELECTOR_MSG_SCREEN)
    {
        popup_track(&msg->u.screen);
    }

    switch (s_menu.state)
    {
    case MENU_STATE_MENU:
        // The Prog menu owns the screen and the buzzer. The backlight still follows the central,
        // because it is not repeated and would be lost when the central is entered.
        if (msg->type == SELECTOR_MSG_BACKLIGHT)
        {
            selector_bridge_emit(SELECTOR_DIR_DOWNSTREAM, msg);
        }
        break;
    case MENU_STATE_STATUS:
        // A valid frame from the central means it is master again
        enter_central(now);
        selector_bridge_emit(SELECTOR_DIR_DOWNSTREAM, msg);
        break;
    case MENU_STATE_CENTRAL:
    case MENU_STATE_REPLAY:
    default:
        selector_bridge_emit(SELECTOR_DIR_DOWNSTREAM, msg);
        break;
    }
}

// A reply went to the central. While recording, counted replies feed the encoder with the key mask
// that was actually sent (the key that picked the entry is masked by handback_mask, so it is not
// recorded).
static void menu_on_reply(void *ctx, uint8_t keys, bool counted)
{
    (void)ctx;
    if (s_menu.recording && counted && !selector_macro_enc_reply(&s_enc, keys))
    {
        ESP_LOGW(TAG, "Recording buffer full: recording ends");
        enter_menu(selector_bridge_now_ms());
    }
}

static void menu_on_link(void *ctx, selector_side_t side, bool up)
{
    (void)ctx;
    // The selector may have been reset or replugged: draw the menu again
    if (side == SELECTOR_SIDE_SELECTOR && up)
    {
        s_menu.dirty = true;
    }
    // Central side: the panel link is a session (open gate), not presence. Presence and the stored
    // pop-up are handled in menu_on_msg and menu_on_tick, so nothing to do here.
}

static void menu_on_mode(void *ctx, bool central_slave)
{
    (void)ctx;
    if (central_slave)
    {
        if (s_menu.recording)
        {
            recording_finish(); // unreachable until master mode exists
        }
        menu_set_state(MENU_STATE_STATUS);
        emit_screen("PROGRAMMING...", "");
        emit_keys(0);
    }
    else if (s_menu.state == MENU_STATE_STATUS)
    {
        // Back to the central: the exit rules apply if it does not talk again
        enter_central(selector_bridge_now_ms());
    }
}

static void menu_on_tick(void *ctx, uint32_t now_ms)
{
    (void)ctx;

    if (s_menu.store_pending)
    {
        store_try();
    }
    claim_service();

    if (!s_menu.started)
    {
        // Boot: the menu shows and the gate closes, the reset procedure waits for the pop-up.
        // (The gate is open from the port start until this first tick, a few milliseconds.)
        s_menu.started = true;
        selector_bridge_set_reply_gate(false);
        reset_begin(now_ms);
    }

    // Presence, in every state: lost after CENTRAL_SILENCE_MS without a valid central frame.
    // Only a loss (or the reset timeout) clears the stored pop-up.
    if (s_menu.central_present && (uint32_t)(now_ms - s_menu.last_central_ms) >= CENTRAL_SILENCE_MS)
    {
        ESP_LOGI(TAG, "Central lost: pop-up cleared");
        s_menu.central_present = false;
        s_menu.reset_active = false;
        s_menu.capture = CAPTURE_NONE;
        popup_clear();
    }

    // A key that is held keeps the key idle timer restarting until it is released.
    if (s_menu.state == MENU_STATE_CENTRAL && s_menu.user_keys != 0)
    {
        s_menu.last_key_ms = now_ms;
    }
    // Exit rules. Both values confirmed on the bench (open questions D2, F8): 3 s of central silence,
    // 3 s with no key pressed (also while recording).
    if ((s_menu.state == MENU_STATE_CENTRAL || s_menu.state == MENU_STATE_REPLAY) &&
        (uint32_t)(now_ms - s_menu.last_central_ms) >= CENTRAL_SILENCE_MS)
    {
        enter_menu(now_ms);
    }
    else if (s_menu.state == MENU_STATE_CENTRAL &&
             (uint32_t)(now_ms - s_menu.last_key_ms) >= CENTRAL_KEY_IDLE_MS)
    {
        enter_menu(now_ms);
    }

    if (s_menu.state == MENU_STATE_MENU)
    {
        if (s_menu.reset_active && (uint32_t)(now_ms - s_menu.reset_start_ms) >= POPUP_RESET_WAIT_MS)
        {
            // The central did not show its start screen in time
            ESP_LOGW(TAG, "Reset wait timed out: pop-up cleared");
            s_menu.reset_active = false;
            s_menu.capture = CAPTURE_NONE; // a failed capture must not stay armed
            popup_clear();
        }

        bool entry_page = s_menu.page >= PAGE_ENTER && s_menu.page < PAGE_IFACE_FIRST;
        uint32_t poll_ms = entry_page ? ENTRY_POLL_MS : NET_POLL_MS;
        if (s_menu.dirty)
        {
            s_menu.dirty = false;
            s_menu.last_net_ms = now_ms;
            menu_show(true);
        }
        else if ((uint32_t)(now_ms - s_menu.last_net_ms) >= poll_ms)
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
    .on_reply = menu_on_reply, // recording (milestone B); replay (milestone C) will use it too
    .gate_closed_at_start = true, // the menu keeps the central silent from the first frame
};

const selector_translator_t *selector_tr_menu(void)
{
    return &s_menu_translator;
}