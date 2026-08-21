#include "settings.h"

#include <stdio.h>
#include <string.h>

#include "app_registry.h"
#include "bsp.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_system.h"
#include "lvgl.h"
#include "net.h"
#include "nvs.h"
#include "ota.h"
#include "shell.h"

static const char *TAG = "settings";

#define NVS_NAMESPACE    "deskos"
#define NVS_KEY_PINNED   "pinned"
#define NVS_KEY_SHOWCASE "showcase"
#define NVS_KEY_THEME    "theme"
#define PINNED_MAX_LEN   256   /* comma-separated app ids, generous */
#define PINNED_MAX_APPS  16

typedef struct {
    const char *name;
    const char *tz;
} tz_preset_t;

/* A short curated list, not freeform entry — there is no keyboard on this
 * device. Add entries here as needed; nothing else references this table. */
static const tz_preset_t TZ_PRESETS[] = {
    { "Sofia/Plovdiv", "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "UTC",           "UTC0" },
    { "London",        "GMT0BST,M3.5.0/1,M10.5.0" },
    { "Berlin",        "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Moscow",        "MSK-3" },
    { "New York",      "EST5EDT,M3.2.0,M11.1.0" },
};
#define TZ_PRESET_COUNT ((int)(sizeof(TZ_PRESETS) / sizeof(TZ_PRESETS[0])))

/* Settings is one screen with swappable content: a category menu plus one
 * sub-view per category. Only one view's widgets exist in s_list at a time —
 * the pointers below are null whenever their view is not the current one, and
 * every reader (refresh_tick, event handlers) must check before touching them. */
typedef enum {
    VIEW_MENU,
    VIEW_ABOUT,
    VIEW_DISPLAY,
    VIEW_NETWORK,
    VIEW_TZ,       /* nested under Network, ROADMAP #36 */
    VIEW_STORAGE,
    VIEW_APPS,
} settings_view_t;

static lv_obj_t   *s_prev_screen;
static lv_obj_t   *s_screen;
static lv_obj_t   *s_list;
static lv_timer_t *s_refresh_timer;

/* Settings' own focus group, separate from the launcher's — sharing one group
 * across screens meant PREV/NEXT kept cycling through the launcher's buttons
 * even while they sat on a different, inactive screen (found on hardware:
 * wrapping from the last Settings row took ~5 extra presses, one per launcher
 * entry, before landing back on the first Settings row). Created once and
 * reused across opens; shell_set_input_group() points the keypad at it while
 * this screen is up and restores the launcher's group when it closes. */
static lv_group_t *s_group;

static lv_obj_t   *s_brightness_slider;   /* Display */
static lv_obj_t   *s_tz_btn;              /* Network */
static lv_obj_t   *s_wifi_btn;            /* Network */
static lv_obj_t   *s_ota_btn;             /* About */
static lv_obj_t   *s_ota_status_label;    /* About */
static lv_obj_t   *s_ram_arc;             /* Storage */
static lv_obj_t   *s_ram_pct_label;       /* Storage */

/* WiFi setup sub-panel — swapped in over the list, torn down on cancel, on a
 * result settling, or on settings_close() (the system "home" shortcut can
 * fire mid-setup and must not strand the device in AP mode). Only ever opened
 * from the Network view. */
static lv_obj_t   *s_wifi_panel;
static lv_obj_t   *s_wifi_status_label;
static lv_timer_t *s_wifi_timer;
static uint8_t     s_wifi_settle_ticks;

/* Delete-confirm sub-panel (ROADMAP #18) — same swap-in-over-the-list
 * pattern as the WiFi panel above, torn down the same three ways. Only ever
 * opened from the Apps view. s_delete_app_id is a copy, not the app_info_t
 * pointer itself: that pointer lives inside the registry's own array and
 * app_registry_delete() + the rescan after it can invalidate it. */
static lv_obj_t *s_delete_panel;
static char      s_delete_app_id[24];
/* The trash button that opened the panel — restored to focus on Cancel (see
 * delete_panel_close()'s own comment for why this can't be left to LVGL). On
 * a confirmed delete this row is about to be destroyed by show_view()'s
 * rebuild anyway, so refocusing it there is harmless, just momentarily moot. */
static lv_obj_t *s_delete_trigger_btn;

/* ---------------------------------------------------------------- pinned set */

static void pinned_read_raw(char *out, size_t out_size)
{
    out[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        size_t n = out_size;
        if (nvs_get_str(h, NVS_KEY_PINNED, out, &n) != ESP_OK) {
            out[0] = '\0';
        }
        nvs_close(h);
    }
}

static int pinned_read_ids(char ids[][24], int max)
{
    char raw[PINNED_MAX_LEN];
    pinned_read_raw(raw, sizeof(raw));

    int n = 0;
    char *save = NULL;
    for (char *tok = strtok_r(raw, ",", &save); tok && n < max; tok = strtok_r(NULL, ",", &save)) {
        snprintf(ids[n], 24, "%s", tok);
        n++;
    }
    return n;
}

static void pinned_write_ids(char ids[][24], int n)
{
    char out[PINNED_MAX_LEN] = "";
    for (int i = 0; i < n; i++) {
        if (out[0]) {
            strncat(out, ",", sizeof(out) - strlen(out) - 1);
        }
        strncat(out, ids[i], sizeof(out) - strlen(out) - 1);
    }

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, NVS_KEY_PINNED, out);
        nvs_commit(h);
        nvs_close(h);
    }
}

bool settings_app_is_pinned(const char *app_id)
{
    if (!app_id || !*app_id) {
        return false;
    }
    char ids[PINNED_MAX_APPS][24];
    const int n = pinned_read_ids(ids, PINNED_MAX_APPS);
    for (int i = 0; i < n; i++) {
        if (strcmp(ids[i], app_id) == 0) {
            return true;
        }
    }
    return false;
}

size_t settings_pinned_apps(const app_info_t **out, size_t max)
{
    char ids[PINNED_MAX_APPS][24];
    const int n = pinned_read_ids(ids, PINNED_MAX_APPS);
    const size_t total = app_registry_count();

    size_t count = 0;
    for (int i = 0; i < n && count < max; i++) {
        for (size_t j = 0; j < total; j++) {
            const app_info_t *app = app_registry_get(j);
            if (strcmp(app->id, ids[i]) == 0) {
                out[count++] = app;
                break;
            }
        }
    }
    return count;
}

/* Read through NVS on every call, same as settings_app_is_pinned() above —
 * shell.c polls this once a second from showcase_tick(), not per frame, so
 * the NVS round-trip is negligible and there's no boot-time init to forget. */
bool settings_showcase_enabled(void)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, NVS_KEY_SHOWCASE, &v);
        nvs_close(h);
    }
    return v != 0;
}

static void settings_set_showcase_enabled(bool enabled)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, NVS_KEY_SHOWCASE, enabled ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* ROADMAP #32. shell_start() reads this once at boot (before any screen
 * exists) so the whole UI comes up already in the right theme; this file
 * reads it again only to draw the switch's initial state. Default true
 * (dark) — matches the look every device had before this setting existed,
 * so upgrading firmware never silently changes anyone's theme. */
bool settings_theme_is_dark(void)
{
    nvs_handle_t h;
    uint8_t v = 1;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, NVS_KEY_THEME, &v);
        nvs_close(h);
    }
    return v != 0;
}

static void settings_set_theme_dark(bool dark)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, NVS_KEY_THEME, dark ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void pinned_toggle(const char *app_id)
{
    char ids[PINNED_MAX_APPS][24];
    int n = pinned_read_ids(ids, PINNED_MAX_APPS);

    int found = -1;
    for (int i = 0; i < n; i++) {
        if (strcmp(ids[i], app_id) == 0) {
            found = i;
            break;
        }
    }
    if (found >= 0) {
        for (int i = found; i < n - 1; i++) {
            memcpy(ids[i], ids[i + 1], sizeof(ids[0]));
        }
        n--;
    } else if (n < PINNED_MAX_APPS) {
        snprintf(ids[n], sizeof(ids[0]), "%s", app_id);
        n++;
    }
    pinned_write_ids(ids, n);
}

/* -------------------------------------------------------------------- rows */

/* Floor at 5, not 0 — 0 turns the backlight fully off with no way to see the
 * slider any more to drag it back up (recovering needs the unrelated
 * system-wide backlight chord, or leaving and reopening Settings — still
 * dark either way). */
#define BRIGHTNESS_MIN 5

/* Live preview while dragging (bsp_backlight_set, cheap — just a PWM duty
 * write), one NVS commit on release — not on every VALUE_CHANGED, which
 * fires continuously mid-drag. Touch-drag path only; button stepping is
 * settings_brightness_step() below. */
static void brightness_slider_event(lv_event_t *e)
{
    const uint8_t pct = (uint8_t)lv_slider_get_value(lv_event_get_target(e));
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
        bsp_backlight_save(pct);
    } else {
        bsp_backlight_set(pct);
    }
}

/* True while the brightness row is toggled into "editing" — see
 * brightness_slider_clicked(). Guards against a stale true surviving past
 * the slider's own destruction (view switch, settings close). */
static bool s_brightness_editing;

bool settings_is_adjusting_brightness(void)
{
    return s_brightness_editing && s_brightness_slider != NULL;
}

/* CLICK-driven, not touch-drag — same NVS-commit-per-step reasoning as the
 * original click-to-cycle brightness row this replaced: a click is a
 * deliberate, infrequent action, not a continuous stream, so committing
 * every step is fine. */
void settings_brightness_step(int delta)
{
    if (!settings_is_adjusting_brightness()) {
        return;
    }
    int pct = (int)bsp_backlight_get() + delta;
    if (pct < BRIGHTNESS_MIN) {
        pct = BRIGHTNESS_MIN;
    } else if (pct > 100) {
        pct = 100;
    }
    bsp_backlight_set((uint8_t)pct);
    bsp_backlight_save((uint8_t)pct);
    lv_slider_set_value(s_brightness_slider, pct, LV_ANIM_OFF);
}

/* The slider behaves like any other menu row for navigation (B1/B2 move
 * focus onto/off it via LV_KEY_PREV/NEXT, same as Back or a Full list tile)
 * but, like a real menu item, needs a "selected" state of its own to become
 * adjustable — this toggles it. Selecting it (B3 click on 3 buttons, B1+B2
 * click on 2) always reaches this: LV_KEY_ENTER is delivered to the focused
 * object regardless of any of this file's own state, and lv_slider treats a
 * no-coordinate keypad "click" as a plain click, not a jump-to-point (that
 * needs a POINTER-indev's touch coordinate, which a simulated ENTER press
 * never carries) — so toggling here never accidentally moves the value. */
static void brightness_slider_clicked(lv_event_t *e)
{
    (void)e;
    s_brightness_editing = !s_brightness_editing;
    if (s_brightness_editing) {
        lv_obj_set_style_border_width(s_brightness_slider, 2, LV_PART_MAIN);
        lv_obj_set_style_border_color(s_brightness_slider, lv_color_hex(0xE8B923), LV_PART_MAIN);
        lv_obj_set_style_border_opa(s_brightness_slider, LV_OPA_COVER, LV_PART_MAIN);
    } else {
        lv_obj_set_style_border_width(s_brightness_slider, 0, LV_PART_MAIN);
    }
}

/* -1 when the effective TZ is not one of the presets (e.g. straight from
 * CONFIG_DESKOS_TZ and nobody has touched this screen yet). */
static int tz_preset_index_of_current(void)
{
    char cur[48];
    net_get_timezone(cur, sizeof(cur));
    for (int i = 0; i < TZ_PRESET_COUNT; i++) {
        if (strcmp(TZ_PRESETS[i].tz, cur) == 0) {
            return i;
        }
    }
    return -1;
}

static void tz_label_update(void)
{
    const int idx = tz_preset_index_of_current();
    char buf[48];
    snprintf(buf, sizeof(buf), "Timezone: %s", idx >= 0 ? TZ_PRESETS[idx].name : "custom");
    lv_list_set_button_text(s_list, s_tz_btn, buf);
}

/* lv_switch instead of a colored checkmark — the checkmark attempt (see git
 * history) read as "selected" rather than "off/dim", and the pinned one's
 * white was unreadable against the row's own focus highlight. A toggle needs
 * no color judgment call: on/off is its whole job. The switch's own click
 * handling flips LV_STATE_CHECKED itself before this fires; this only
 * mirrors that into NVS, and only if it actually disagrees with what is
 * already stored (defensive, not load-bearing — a genuine single click
 * always disagrees). */
static void pin_switch_changed(lv_event_t *e)
{
    const app_info_t *app = lv_event_get_user_data(e);
    const bool checked = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    if (checked != settings_app_is_pinned(app->id)) {
        pinned_toggle(app->id);
    }
}

/* Keeps the WiFi row live while the Network view sits open and idle:
 * net_state() can change in the background (setup finishing, or just the
 * normal reconnect backoff). Without this the row could freeze on
 * "connecting" — seen on hardware right after a SoftAP setup that settled to
 * NET_UP a beat after the setup panel had already closed and stopped
 * watching it. Only valid while s_wifi_btn is non-null (Network view active). */
static void wifi_label_update(void)
{
    char ssid[33], line[64];
    if (net_get_ssid(ssid, sizeof(ssid))) {
        snprintf(line, sizeof(line), "WiFi: %s (%s)", ssid,
                 net_state() == NET_UP ? "connected" : "connecting");
    } else {
        snprintf(line, sizeof(line), "WiFi: not configured (tap to set up)");
    }
    lv_list_set_button_text(s_list, s_wifi_btn, line);
}

/* A bare "NN KB free" is not informative on its own — free of what? — same
 * complaint that sent the storage bars to a Windows-style "X free of Y"
 * caption. RAM has no natural "total" to caption it with the same way (no
 * single obviously-right denominator: heap_caps total, PSRAM-less budget,
 * and "what CLAUDE.md calls free after boot" are all different numbers), so
 * this instead mirrors the built-in `system` app's CPU gauge (see
 * main/builtin/system.ui.jsonl) — an arc + big percentage, same "% used,
 * high is the one to worry about" reading a user has already seen there. */
static void ram_gauge_refresh(void)
{
    if (!s_ram_arc) {
        return;
    }
    const size_t total = heap_caps_get_total_size(MALLOC_CAP_DEFAULT);
    const size_t free = esp_get_free_heap_size();
    const uint8_t pct = total > 0 ? (uint8_t)(((total - free) * 100) / total) : 0;

    lv_arc_set_value(s_ram_arc, pct);
    if (s_ram_pct_label) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%u%%", pct);
        lv_label_set_text(s_ram_pct_label, buf);
    }
}

static void refresh_tick(lv_timer_t *timer)
{
    (void)timer;
    ram_gauge_refresh();
    if (s_wifi_btn) {
        wifi_label_update();
    }
}

/* ---------------------------------------------------------------- WiFi setup */

static void wifi_setup_close(bool user_cancelled)
{
    if (!s_wifi_panel) {
        return;
    }
    if (s_wifi_timer) {
        lv_timer_delete(s_wifi_timer);
        s_wifi_timer = NULL;
    }
    if (user_cancelled) {
        net_softap_stop();
    }
    lv_obj_delete(s_wifi_panel);
    s_wifi_panel = NULL;
    s_wifi_status_label = NULL;
    lv_obj_remove_flag(s_list, LV_OBJ_FLAG_HIDDEN);
    wifi_label_update();
}

/* Polls net_state() rather than reacting to net_on_state_change(): that
 * callback is already claimed by shell.c for the status bar, and one more
 * subscriber would mean unsubscribing correctly on every close path. A
 * 500 ms poll is plenty responsive for a screen a human is watching. */
static void wifi_setup_tick(lv_timer_t *timer)
{
    (void)timer;
    const net_state_t st = net_state();
    const char *msg;
    switch (st) {
    case NET_SOFTAP:     msg = "Waiting for the form to be submitted..."; break;
    case NET_CONNECTING: msg = "Saved. Reconnecting...";                  break;
    case NET_UP:         msg = "Connected!";                              break;
    default:             msg = "Could not connect — check the password."; break;
    }
    lv_label_set_text(s_wifi_status_label, msg);

    if (st == NET_SOFTAP) {
        s_wifi_settle_ticks = 0;
        return;
    }
    /* Setup ended on its own (form submitted and applied). Let the result
     * message sit on screen for a beat before returning to the list. */
    if (++s_wifi_settle_ticks >= 3) {
        wifi_setup_close(false);
    }
}

static void wifi_cancel_clicked(lv_event_t *e)
{
    (void)e;
    wifi_setup_close(true);
}

static void wifi_clicked(lv_event_t *e)
{
    (void)e;
    if (s_wifi_panel) {
        return;
    }
    if (net_softap_start() != ESP_OK) {
        ESP_LOGW(TAG, "failed to start WiFi setup AP");
        return;
    }
    s_wifi_settle_ticks = 0;

    lv_obj_add_flag(s_list, LV_OBJ_FLAG_HIDDEN);

    s_wifi_panel = lv_obj_create(s_screen);
    lv_obj_set_size(s_wifi_panel, BSP_LCD_H_RES - 24, BSP_LCD_V_RES - 24 - SHELL_TOOLBAR_HEIGHT);
    lv_obj_align(s_wifi_panel, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_set_flex_flow(s_wifi_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(s_wifi_panel, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_row(s_wifi_panel, 10, LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(s_wifi_panel);
    lv_label_set_text(title, "WiFi setup");

    char instr[112];
    snprintf(instr, sizeof(instr), "1. Connect a phone to WiFi \"%s\"\n2. Open %s in a browser",
             NET_SOFTAP_SSID, NET_SOFTAP_URL);
    lv_obj_t *instr_lbl = lv_label_create(s_wifi_panel);
    lv_obj_set_width(instr_lbl, LV_PCT(100));
    lv_label_set_long_mode(instr_lbl, LV_LABEL_LONG_WRAP);
    lv_label_set_text(instr_lbl, instr);

    s_wifi_status_label = lv_label_create(s_wifi_panel);
    lv_obj_set_width(s_wifi_status_label, LV_PCT(100));
    lv_label_set_long_mode(s_wifi_status_label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_wifi_status_label, "Waiting for the form to be submitted...");

    lv_obj_t *cancel_btn = lv_button_create(s_wifi_panel);
    lv_obj_t *cancel_lbl = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_lbl, "Cancel");
    lv_obj_add_event_cb(cancel_btn, wifi_cancel_clicked, LV_EVENT_CLICKED, NULL);
    lv_group_add_obj(s_group, cancel_btn);

    s_wifi_timer = lv_timer_create(wifi_setup_tick, 500, NULL);
}

/* --------------------------------------------------------------------- OTA */

/* Runs on the OTA task, not the LVGL one — must take the lock itself, same
 * convention as net.c/host.c's own state-change callbacks. Guards against
 * s_ota_status_label already being gone (settings screen closed, or just not
 * on the About view any more) by failing the lock/null-check silently; the
 * OTA task itself does not care whether anyone is listening. */
static void ota_status_cb(ota_state_t state, int percent, const char *detail)
{
    char buf[64];
    switch (state) {
    case OTA_CONNECTING:
        snprintf(buf, sizeof(buf), "OTA: connecting...");
        break;
    case OTA_DOWNLOADING:
        if (detail) {
            snprintf(buf, sizeof(buf), "OTA: downloading %s...", detail);
        } else {
            snprintf(buf, sizeof(buf), "OTA: downloading %d%%", percent);
        }
        break;
    case OTA_DONE:
        snprintf(buf, sizeof(buf), "OTA: done, rebooting...");
        break;
    case OTA_ERROR:
        snprintf(buf, sizeof(buf), "OTA failed: %s", detail ? detail : "?");
        break;
    default:
        buf[0] = '\0';
        break;
    }

    if (lvgl_port_lock(200)) {
        if (s_ota_status_label) {
            lv_label_set_text(s_ota_status_label, buf);
        }
        lvgl_port_unlock();
    }
}

static void ota_clicked(lv_event_t *e)
{
    (void)e;
    if (ota_is_running()) {
        return;
    }
    ota_check_and_update(ota_status_cb);
}

/* ------------------------------------------------------------------ views */

static void show_view(settings_view_t view);

/* Also used directly by category_clicked below — "Back" is just a jump to a
 * fixed target view, the same thing a Full list category row does with its
 * own target. ROADMAP #36 needed a "Back" that returns to Network rather
 * than always Menu (a nested list under Network, not a top-level category),
 * which is why this takes the target as a parameter instead of hardcoding
 * VIEW_MENU the way it used to. */
static void category_clicked(lv_event_t *e)
{
    const settings_view_t view = (settings_view_t)(intptr_t)lv_event_get_user_data(e);
    show_view(view);
}

static void add_back_row(settings_view_t target)
{
    lv_obj_t *btn = lv_list_add_button(s_list, LV_SYMBOL_LEFT, "Back");
    lv_obj_add_event_cb(btn, category_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)target);
    lv_group_add_obj(s_group, btn);
}

/* ROADMAP #36: was a click-to-cycle-through-presets row; a real list (one
 * row per preset, current one marked) reads better once there are more than
 * two or three presets to choose from and matches how every other
 * multiple-choice setting on this screen already works. */
static void tz_clicked(lv_event_t *e)
{
    (void)e;
    show_view(VIEW_TZ);
}

static void tz_preset_clicked(lv_event_t *e)
{
    const int idx = (int)(intptr_t)lv_event_get_user_data(e);
    net_set_timezone(TZ_PRESETS[idx].tz);
    show_view(VIEW_NETWORK);
}

static void build_tz(void)
{
    add_back_row(VIEW_NETWORK);

    const int current = tz_preset_index_of_current();
    for (int i = 0; i < TZ_PRESET_COUNT; i++) {
        lv_obj_t *btn = lv_list_add_button(s_list, i == current ? LV_SYMBOL_OK : NULL, TZ_PRESETS[i].name);
        lv_obj_add_event_cb(btn, tz_preset_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_group_add_obj(s_group, btn);
    }
}

static void build_menu(void)
{
    static const struct { const char *label; settings_view_t view; } items[] = {
        { "About device", VIEW_ABOUT },
        { "Display",      VIEW_DISPLAY },
        { "Network",      VIEW_NETWORK },
        { "Storage",      VIEW_STORAGE },
        { "Apps",         VIEW_APPS },
    };
    lv_group_t *group = s_group;
    for (size_t i = 0; i < sizeof(items) / sizeof(items[0]); i++) {
        lv_obj_t *btn = lv_list_add_button(s_list, NULL, items[i].label);
        lv_obj_add_event_cb(btn, category_clicked, LV_EVENT_CLICKED,
                            (void *)(intptr_t)items[i].view);
        lv_group_add_obj(group, btn);
    }
}

/* lv_list_add_text()'s default LVGL theme styling paints a grey background
 * block behind the text (lv_theme_default.c's lv_list_text_class handling) —
 * every plain info line on this screen reads as its own separate chip
 * instead of just being text. Every info/status line below goes through
 * this instead of the raw call so none of them get that box; a caller that
 * also wants a non-default text color (e.g. a warning) still sets that
 * itself on the object this returns. */
static lv_obj_t *list_text(const char *txt)
{
    lv_obj_t *label = lv_list_add_text(s_list, txt);
    lv_obj_set_style_bg_opa(label, LV_OPA_TRANSP, LV_PART_MAIN);
    /* This device's only way to scroll IS moving keyboard focus — there is
     * no separate scroll gesture on a 2-button board. Two things are needed
     * for that, not just one: membership in s_group (so PREV/NEXT can land
     * here at all) AND LV_OBJ_FLAG_SCROLL_ON_FOCUS (so landing here actually
     * scrolls it into view — lv_obj.c only calls lv_obj_scroll_to_view_
     * recursive() on LV_EVENT_FOCUSED when this flag is set; it is NOT
     * something group membership grants for free). lv_button/lv_switch/
     * lv_slider all set this themselves in their own constructors, which is
     * exactly why every other row on every other view already scrolled
     * correctly without anyone noticing this was two separate mechanisms —
     * a plain lv_obj/lv_label/lv_bar/lv_arc sets neither on its own.
     * Harmless for a row with no click handler: ENTER on it just does
     * nothing, same as landing on any other inert row would. */
    lv_group_add_obj(s_group, label);
    lv_obj_add_flag(label, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    return label;
}

/* A thin rule between sections of the same view (SD / internal storage /
 * memory on Storage) — plain lv_obj, not a list row, so it takes no part in
 * button navigation. */
static void add_divider(void)
{
    lv_obj_t *d = lv_obj_create(s_list);
    lv_obj_remove_flag(d, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(d, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_width(d, LV_PCT(100));
    lv_obj_set_height(d, 1);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(d, lv_palette_main(LV_PALETTE_GREY), LV_PART_MAIN);
    lv_obj_set_style_border_width(d, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(d, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(d, 0, LV_PART_MAIN);
    lv_obj_set_style_margin_top(d, 6, LV_PART_MAIN);
    lv_obj_set_style_margin_bottom(d, 6, LV_PART_MAIN);
}

/* Logo placeholder — no real asset yet (see ROADMAP #28 on the lack of any
 * icon pipeline at all), just a coloured circle reserving the spot at the
 * top of the device identity screen. */
static void build_about(void)
{
    add_back_row(VIEW_MENU);

    lv_obj_t *logo_row = lv_obj_create(s_list);
    lv_obj_remove_flag(logo_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(logo_row, LV_PCT(100));
    lv_obj_set_height(logo_row, 64);
    lv_obj_set_style_bg_opa(logo_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(logo_row, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(logo_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(logo_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *logo = lv_obj_create(logo_row);
    lv_obj_remove_flag(logo, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(logo, 48, 48);
    lv_obj_set_style_radius(logo, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(logo, lv_palette_main(LV_PALETTE_DEEP_PURPLE), LV_PART_MAIN);
    lv_obj_set_style_border_width(logo, 0, LV_PART_MAIN);

    const esp_app_desc_t *desc = esp_app_get_description();
    char line[64];

    snprintf(line, sizeof(line), "Device: %s", desc->project_name);
    list_text(line);

    snprintf(line, sizeof(line), "Firmware: %s", desc->version);
    list_text(line);

    s_ota_btn = lv_list_add_button(s_list, NULL, "Check for update");
    lv_obj_add_event_cb(s_ota_btn, ota_clicked, LV_EVENT_CLICKED, NULL);
    lv_group_add_obj(s_group, s_ota_btn);
    s_ota_status_label = list_text("");
}

/* LVGL's built-in symbol font (lv_symbol_def.h) has no sun/brightness glyph,
 * and the project deliberately never embeds custom font or image assets (see
 * CLAUDE.md, "Что НЕ делать") — so this is drawn from primitives instead of
 * picked out of an icon set: a small circle plus a horizontal/vertical ray
 * behind it. A stopgap ahead of ROADMAP #34 (real icon set on SD); swap for
 * a proper asset there. lv_list_add_button() can't host it — its icon slot
 * only takes an image/symbol source, not an arbitrary child object — so the
 * row is built by hand instead, matching what that helper does internally. */
static void sun_icon_create(lv_obj_t *parent)
{
    static const lv_point_precise_t h_ray[] = { { 0, 8 }, { 16, 8 } };
    static const lv_point_precise_t v_ray[] = { { 8, 0 }, { 8, 16 } };
    const lv_color_t sun_color = lv_color_hex(0xE8B923);

    lv_obj_t *icon = lv_obj_create(parent);
    lv_obj_remove_flag(icon, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(icon, 16, 16);
    lv_obj_set_style_bg_opa(icon, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(icon, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(icon, 0, LV_PART_MAIN);

    lv_obj_t *h = lv_line_create(icon);
    lv_line_set_points(h, h_ray, 2);
    lv_obj_set_style_line_color(h, sun_color, LV_PART_MAIN);
    lv_obj_set_style_line_width(h, 2, LV_PART_MAIN);

    lv_obj_t *v = lv_line_create(icon);
    lv_line_set_points(v, v_ray, 2);
    lv_obj_set_style_line_color(v, sun_color, LV_PART_MAIN);
    lv_obj_set_style_line_width(v, 2, LV_PART_MAIN);

    lv_obj_t *core = lv_obj_create(icon);
    lv_obj_remove_flag(core, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(core, 8, 8);
    lv_obj_center(core);
    lv_obj_set_style_radius(core, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(core, sun_color, LV_PART_MAIN);
    lv_obj_set_style_border_width(core, 0, LV_PART_MAIN);
}

/* Flips LVGL's default theme plus the shell's own explicitly-colored root
 * screens (shell_apply_theme(), see shell.h for why the split is needed),
 * then re-colors this screen's own background the same way — s_screen is
 * exactly the kind of local override shell_apply_theme() cannot reach on its
 * own, since it lives in this file, not shell.c. */
static void theme_switch_changed(lv_event_t *e)
{
    const bool dark = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    settings_set_theme_dark(dark);
    shell_apply_theme(dark);
    lv_obj_set_style_bg_color(s_screen, shell_theme_bg(), LV_PART_MAIN);
}

static void build_display(void)
{
    add_back_row(VIEW_MENU);

    /* Caption only — no event handler, not added to s_group, so it is not a
     * focus/click target, just an icon+label for the slider row below it. */
    lv_obj_t *brightness_row = lv_obj_create(s_list);
    lv_obj_remove_flag(brightness_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(brightness_row, LV_PCT(100));
    lv_obj_set_height(brightness_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(brightness_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(brightness_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(brightness_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(brightness_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_column(brightness_row, 8, LV_PART_MAIN);

    sun_icon_create(brightness_row);
    lv_obj_t *brightness_label = lv_label_create(brightness_row);
    lv_label_set_text(brightness_label, "Brightness");

    s_brightness_editing = false;
    s_brightness_slider = lv_slider_create(s_list);
    lv_obj_set_width(s_brightness_slider, LV_PCT(100));
    /* The list's own item spacing (pad_gap) is 0 — normal list rows butt
     * against each other with only their own bottom border between them,
     * but this slider is a plain object, not a list row, and its round knob
     * draws ~6px past its own box on every side (LVGL default theme's knob
     * style pads the circle out past the track). With zero gap that overflow
     * bled straight into whatever sat right below it — found on hardware
     * once the theme switch below gave it a neighbour for the first time
     * (previously the slider was the last row, so it only overflowed into
     * the list's own bottom padding, empty space no one saw). A margin, not
     * padding — padding would just add inside the slider's own box, which
     * the knob already draws past regardless. */
    lv_obj_set_style_margin_bottom(s_brightness_slider, 10, LV_PART_MAIN);
    lv_slider_set_range(s_brightness_slider, BRIGHTNESS_MIN, 100);
    lv_slider_set_value(s_brightness_slider, bsp_backlight_get(), LV_ANIM_OFF);
    lv_obj_add_event_cb(s_brightness_slider, brightness_slider_event, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_brightness_slider, brightness_slider_event, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_brightness_slider, brightness_slider_clicked, LV_EVENT_CLICKED, NULL);
    lv_group_add_obj(s_group, s_brightness_slider);

    /* Same label+switch shape as Apps' Showcase mode row below — a toggle
     * needs no contrast judgment call, unlike the checkmark this project
     * already moved away from once (see pin_switch_changed's comment). */
    lv_obj_t *theme_row = lv_list_add_button(s_list, NULL, NULL);
    lv_obj_remove_flag(theme_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *theme_label = lv_label_create(theme_row);
    lv_obj_set_flex_grow(theme_label, 1);
    lv_label_set_text(theme_label, "Dark theme");
    lv_obj_t *theme_sw = lv_switch_create(theme_row);
    lv_obj_set_size(theme_sw, 28, 14);
    if (settings_theme_is_dark()) {
        lv_obj_add_state(theme_sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(theme_sw, theme_switch_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_group_add_obj(s_group, theme_sw);
}

static void build_network(void)
{
    add_back_row(VIEW_MENU);

    s_tz_btn = lv_list_add_button(s_list, NULL, "");
    lv_obj_add_event_cb(s_tz_btn, tz_clicked, LV_EVENT_CLICKED, NULL);
    lv_group_add_obj(s_group, s_tz_btn);
    tz_label_update();

    s_wifi_btn = lv_list_add_button(s_list, NULL, "");
    lv_obj_add_event_cb(s_wifi_btn, wifi_clicked, LV_EVENT_CLICKED, NULL);
    lv_group_add_obj(s_group, s_wifi_btn);
    wifi_label_update();
}

/* KB is fine for the internal LittleFS partition (hundreds of KB, see
 * CLAUDE.md) but useless for an SD card sized in gigabytes — "1900544 KB
 * free" is not a number anyone reads at a glance. Auto-picks the largest
 * unit that keeps at least one whole unit, same rule any file manager uses. */
static void format_kb(size_t kb, char *out, size_t out_size)
{
    if (kb < 1024) {
        snprintf(out, out_size, "%u KB", (unsigned)kb);
    } else if (kb < 1024 * 1024) {
        snprintf(out, out_size, "%.1f MB", kb / 1024.0);
    } else {
        snprintf(out, out_size, "%.1f GB", kb / (1024.0 * 1024.0));
    }
}

/* ROADMAP #37 — Windows Explorer's drive gauge shape, by direct request:
 * a full-width bar (filled portion = used) with "X free of Y" printed below
 * it, no percentage anywhere. Plain lv_bar, not added to s_group: a
 * read-only gauge, nothing to select or step with B1/B2. */
static void add_usage_bar(size_t used_kb, size_t total_kb)
{
    const uint8_t pct = total_kb > 0 ? (uint8_t)((used_kb * 100) / total_kb) : 0;
    const size_t free_kb = total_kb > used_kb ? total_kb - used_kb : 0;

    lv_obj_t *bar = lv_bar_create(s_list);
    lv_obj_set_width(bar, LV_PCT(100));
    lv_obj_set_height(bar, 10);
    lv_obj_set_style_margin_top(bar, 6, LV_PART_MAIN);
    lv_obj_set_style_margin_bottom(bar, 6, LV_PART_MAIN);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, pct, LV_ANIM_OFF);
    lv_group_add_obj(s_group, bar);   /* see list_text()'s comment: both calls needed */
    lv_obj_add_flag(bar, LV_OBJ_FLAG_SCROLL_ON_FOCUS);

    char free_str[16], total_str[16], buf[40];
    format_kb(free_kb, free_str, sizeof(free_str));
    format_kb(total_kb, total_str, sizeof(total_str));
    snprintf(buf, sizeof(buf), "%s free of %s", free_str, total_str);
    list_text(buf);
}

/* Same arc-plus-big-number shape as the built-in `system` app's CPU gauge
 * (main/builtin/system.ui.jsonl) — see ram_gauge_refresh()'s own comment for
 * why a percentage, not a bare KB figure. Default lv_arc angles (135°..45°,
 * gap at the bottom) are the same speedometer look that widget.c's own arc
 * view relies on for the CPU gauge — nothing to set explicitly for that. */
static void add_memory_gauge(void)
{
    lv_obj_t *row = lv_obj_create(s_list);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_bottom(row, 6, LV_PART_MAIN);
    lv_group_add_obj(s_group, row);   /* see list_text()'s comment: both calls needed */
    lv_obj_add_flag(row, LV_OBJ_FLAG_SCROLL_ON_FOCUS);

    s_ram_arc = lv_arc_create(row);
    lv_obj_set_size(s_ram_arc, 90, 90);
    lv_arc_set_range(s_ram_arc, 0, 100);
    lv_arc_set_mode(s_ram_arc, LV_ARC_MODE_NORMAL);
    /* A passive readout, not a control — nothing here should be draggable
     * under the resistive touch (same reasoning as widget.c's own arc view). */
    lv_obj_remove_flag(s_ram_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_color(s_ram_arc, lv_palette_main(LV_PALETTE_DEEP_PURPLE), LV_PART_INDICATOR);

    s_ram_pct_label = lv_label_create(s_ram_arc);
    lv_obj_center(s_ram_pct_label);

    lv_obj_t *caption = lv_label_create(row);
    lv_label_set_text(caption, "RAM used");

    ram_gauge_refresh();
}

static void build_storage(void)
{
    add_back_row(VIEW_MENU);

    char line[64];

    add_memory_gauge();
    add_divider();

    /* No card at all is not worth a line of its own here — Full list/Apps
     * already say so wherever the absence actually matters (no apps to
     * scan, nothing to pin). This screen just shows what there is to show. */
    if (bsp_sd_is_mounted()) {
        char name[32];
        uint32_t capacity_mb = 0;
        bsp_sd_info(name, sizeof(name), &capacity_mb);
        snprintf(line, sizeof(line), "SD: %s, %lu MB", name, (unsigned long)capacity_mb);
        list_text(line);

        size_t sd_used_kb = 0, sd_total_kb = 0;
        bsp_sd_usage(&sd_used_kb, &sd_total_kb);
        add_usage_bar(sd_used_kb, sd_total_kb);

        add_divider();
    }

    size_t used_kb = 0, total_kb = 0;
    bsp_fs_usage(&used_kb, &total_kb);
    list_text("Internal storage");
    add_usage_bar(used_kb, total_kb);
}

/* The actual scan (blocking SD card I/O — main.c's own boot-time call is not
 * instant either) runs one timer tick after the click, not inside it: this
 * lets LVGL paint the spinner added below first. Doing the scan straight in
 * rescan_clicked() would block the same task that draws the spinner, so the
 * spinner (and the click itself) would never visibly render before the scan
 * had already finished — found on hardware as "nothing happens on click". */
static void rescan_do(lv_timer_t *timer)
{
    (void)timer;
    app_registry_scan();
    shell_refresh_app_list();
    show_view(VIEW_APPS);
}

static void rescan_clicked(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    lv_list_set_button_text(s_list, btn, "Scanning...");
    lv_obj_add_state(btn, LV_STATE_DISABLED);   /* one rescan at a time */

    lv_obj_t *spinner = lv_spinner_create(btn);
    lv_obj_set_size(spinner, 16, 16);
    lv_obj_align(spinner, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_obj_remove_flag(spinner, LV_OBJ_FLAG_CLICKABLE);

    lv_timer_t *t = lv_timer_create(rescan_do, 30, NULL);
    lv_timer_set_repeat_count(t, 1);
}

/* Deleting s_delete_panel removes its focused button (Cancel or Delete) from
 * s_group, and LVGL's own auto-refocus-on-removal runs at that exact moment
 * — while s_list is still hidden, since the unhide below hasn't happened
 * yet — so it walks the whole group, finds every candidate hidden (a hidden
 * object never receives focus, see lv_group.c), and gives up with no focus
 * at all. Found on hardware as "list navigation dead after Cancel": nothing
 * was focused any more for B1/B2 to move away from. Explicitly refocusing
 * the trash button that opened this panel, now that s_list is visible
 * again, is the fix. */
static void delete_panel_close(void)
{
    if (!s_delete_panel) {
        return;
    }
    lv_obj_delete(s_delete_panel);
    s_delete_panel = NULL;
    lv_obj_remove_flag(s_list, LV_OBJ_FLAG_HIDDEN);
    if (s_delete_trigger_btn) {
        lv_group_focus_obj(s_delete_trigger_btn);
        s_delete_trigger_btn = NULL;
    }
}

static void delete_cancel_clicked(lv_event_t *e)
{
    (void)e;
    delete_panel_close();
}

static void delete_confirm_clicked(lv_event_t *e)
{
    (void)e;
    app_registry_delete(s_delete_app_id);
    app_registry_scan();
    shell_refresh_app_list();
    delete_panel_close();
    show_view(VIEW_APPS);
}

static void delete_clicked(lv_event_t *e)
{
    const app_info_t *app = lv_event_get_user_data(e);
    if (s_delete_panel) {
        return;
    }
    snprintf(s_delete_app_id, sizeof(s_delete_app_id), "%s", app->id);
    s_delete_trigger_btn = lv_event_get_target(e);

    lv_obj_add_flag(s_list, LV_OBJ_FLAG_HIDDEN);

    s_delete_panel = lv_obj_create(s_screen);
    lv_obj_set_size(s_delete_panel, BSP_LCD_H_RES - 24, BSP_LCD_V_RES - 24 - SHELL_TOOLBAR_HEIGHT);
    lv_obj_align(s_delete_panel, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_set_flex_flow(s_delete_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(s_delete_panel, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_row(s_delete_panel, 10, LV_PART_MAIN);

    char msg[96];
    snprintf(msg, sizeof(msg), "Delete '%s'? This cannot be undone.", app->name);
    lv_obj_t *msg_lbl = lv_label_create(s_delete_panel);
    lv_obj_set_width(msg_lbl, LV_PCT(100));
    lv_obj_set_flex_grow(msg_lbl, 1);   /* pushes btn_row down to the panel's bottom edge */
    lv_label_set_long_mode(msg_lbl, LV_LABEL_LONG_WRAP);
    lv_label_set_text(msg_lbl, msg);

    /* Cancel and Delete side by side, right-aligned within their own row —
     * the panel's own COLUMN flow can't right-align one child without also
     * right-aligning msg_lbl above it, so the row handles its own alignment
     * instead of relying on the panel's. */
    lv_obj_t *btn_row = lv_obj_create(s_delete_panel);
    lv_obj_remove_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);
    /* The focused button's own outline draws a few px past its edge — with
     * btn_row sized tight to content (LV_SIZE_CONTENT, zero padding) and
     * LVGL's default child clipping, that outline was getting cut off at
     * btn_row's own bounds. OVERFLOW_VISIBLE lets it draw past them. */
    lv_obj_add_flag(btn_row, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_set_width(btn_row, LV_PCT(100));
    lv_obj_set_height(btn_row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(btn_row, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(btn_row, 8, LV_PART_MAIN);

    lv_obj_t *cancel_btn = lv_button_create(btn_row);
    lv_obj_t *cancel_lbl = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_lbl, "Cancel");
    lv_obj_add_event_cb(cancel_btn, delete_cancel_clicked, LV_EVENT_CLICKED, NULL);
    lv_group_add_obj(s_group, cancel_btn);

    lv_obj_t *confirm_btn = lv_button_create(btn_row);
    lv_obj_set_style_bg_color(confirm_btn, lv_color_hex(0xC0392B), LV_PART_MAIN);
    lv_obj_t *confirm_lbl = lv_label_create(confirm_btn);
    lv_label_set_text(confirm_lbl, "Delete");
    lv_obj_add_event_cb(confirm_btn, delete_confirm_clicked, LV_EVENT_CLICKED, NULL);
    lv_group_add_obj(s_group, confirm_btn);

    /* Default focus to the safe choice — unlike the WiFi panel above, this
     * one is destructive, so it is worth the one extra call rather than
     * leaving focus wherever it happened to sit before the panel opened. */
    lv_group_focus_obj(cancel_btn);
}

/* ROADMAP #19: auto-advance through pinned widgets on Home. shell.c owns the
 * actual cycling (showcase_tick(), polls settings_showcase_enabled() once a
 * second) — this is just the on/off switch, same lv_switch pattern as the
 * pin-to-Home toggle below, for the same reason (a colored checkmark is a
 * contrast judgment call, a toggle isn't). */
static void showcase_switch_changed(lv_event_t *e)
{
    settings_set_showcase_enabled(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}

static void build_apps(void)
{
    add_back_row(VIEW_MENU);

    lv_obj_t *rescan_btn = lv_list_add_button(s_list, LV_SYMBOL_REFRESH, "Rescan SD card");
    lv_obj_add_event_cb(rescan_btn, rescan_clicked, LV_EVENT_CLICKED, NULL);
    lv_group_add_obj(s_group, rescan_btn);

    /* A manifest.json that failed to load (ROADMAP #18) — a directory with
     * no manifest.json at all is not reported here, see app_registry.h. */
    const size_t err_n = app_registry_error_count();
    for (size_t i = 0; i < err_n; i++) {
        char dir[32], reason[48], line[96];
        app_registry_get_error(i, dir, sizeof(dir), reason, sizeof(reason));
        snprintf(line, sizeof(line), "%s %s: %s", LV_SYMBOL_WARNING, dir, reason);
        lv_obj_t *warn = list_text(line);
        lv_obj_set_style_text_color(warn, lv_color_hex(0xc9a227), LV_PART_MAIN);
    }

    lv_group_t *group = s_group;

    lv_obj_t *showcase_row = lv_list_add_button(s_list, NULL, NULL);
    lv_obj_remove_flag(showcase_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *showcase_label = lv_label_create(showcase_row);
    lv_obj_set_flex_grow(showcase_label, 1);
    lv_label_set_text(showcase_label, "Showcase mode (auto-advance Home)");
    lv_obj_t *showcase_sw = lv_switch_create(showcase_row);
    lv_obj_set_size(showcase_sw, 28, 14);
    if (settings_showcase_enabled()) {
        lv_obj_add_state(showcase_sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(showcase_sw, showcase_switch_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_group_add_obj(group, showcase_sw);

    list_text("Pinned on Home:");
    const size_t n = app_registry_count();
    for (size_t i = 0; i < n; i++) {
        const app_info_t *app = app_registry_get(i);

        /* A real list button, same as every other row on this screen (Back,
         * Rescan, TZ, WiFi...) — a hand-built container here (tried once,
         * see git history) picks up none of the list's flat row styling and
         * just looks like a floating rounded box. txt is NULL, not the app
         * name: that would insert an auto-created label as child 0, ahead of
         * the switch this row needs first. Not clickable itself any more,
         * either — it is just a row container now; the switch, name label,
         * and (for SD apps) the trash button are the real, independent click
         * targets, added by hand in that order. None of them bubbles a click
         * up to the row (LVGL only bubbles on request), so they stay
         * independent of each other too. */
        lv_obj_t *row = lv_list_add_button(s_list, NULL, NULL);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *pin_sw = lv_switch_create(row);
        lv_obj_set_size(pin_sw, 28, 14);   /* the default switch dwarfs a 14px row */
        if (settings_app_is_pinned(app->id)) {
            lv_obj_add_state(pin_sw, LV_STATE_CHECKED);
        }
        lv_obj_add_event_cb(pin_sw, pin_switch_changed, LV_EVENT_VALUE_CHANGED, (void *)app);
        lv_group_add_obj(group, pin_sw);

        lv_obj_t *name_lbl = lv_label_create(row);
        lv_obj_set_flex_grow(name_lbl, 1);
        lv_label_set_text(name_lbl, app->name);

        /* Built-ins are re-extracted from firmware at every boot — deleting
         * one would just be undone on the next reboot, so no trash button. */
        const bool deletable = strncmp(app->dir, BSP_SD_MOUNT_POINT, strlen(BSP_SD_MOUNT_POINT)) == 0;
        if (deletable) {
            /* No background/border of its own — just a red glyph — and sized
             * to the glyph alone (LV_SIZE_CONTENT + zero padding) rather
             * than a default button's box, so it does not stretch the row
             * taller than the name label next to it. */
            lv_obj_t *del_btn = lv_button_create(row);
            lv_obj_set_size(del_btn, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_obj_set_style_bg_opa(del_btn, LV_OPA_TRANSP, LV_PART_MAIN);
            lv_obj_set_style_border_width(del_btn, 0, LV_PART_MAIN);
            lv_obj_set_style_shadow_width(del_btn, 0, LV_PART_MAIN);
            lv_obj_set_style_pad_all(del_btn, 0, LV_PART_MAIN);
            lv_obj_set_style_radius(del_btn, 0, LV_PART_MAIN);   /* square focus outline */
            lv_obj_t *del_lbl = lv_label_create(del_btn);
            lv_label_set_text(del_lbl, LV_SYMBOL_TRASH);
            lv_obj_set_style_text_color(del_lbl, lv_color_hex(0xC0392B), LV_PART_MAIN);
            lv_obj_add_event_cb(del_btn, delete_clicked, LV_EVENT_CLICKED, (void *)app);
            lv_group_add_obj(group, del_btn);
        }
    }
}

static void show_view(settings_view_t view)
{
    lv_obj_clean(s_list);
    s_brightness_slider = NULL;
    s_brightness_editing = false;
    s_tz_btn = NULL;
    s_wifi_btn = NULL;
    s_ota_btn = NULL;
    s_ota_status_label = NULL;
    s_ram_arc = NULL;
    s_ram_pct_label = NULL;

    switch (view) {
    case VIEW_ABOUT:   build_about();   break;
    case VIEW_DISPLAY: build_display(); break;
    case VIEW_NETWORK: build_network(); break;
    case VIEW_TZ:      build_tz();      break;
    case VIEW_STORAGE: build_storage(); break;
    case VIEW_APPS:    build_apps();    break;
    case VIEW_MENU:
    default:           build_menu();    break;
    }

    refresh_tick(NULL);
}

/* ------------------------------------------------------------------- public */

esp_err_t settings_open(void)
{
    if (s_screen) {
        return ESP_OK;
    }

    s_prev_screen = lv_screen_active();
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, shell_theme_bg(), LV_PART_MAIN);
    lv_obj_remove_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    s_list = lv_list_create(s_screen);
    lv_obj_set_size(s_list, BSP_LCD_H_RES - 12, BSP_LCD_V_RES - 12 - SHELL_TOOLBAR_HEIGHT);
    lv_obj_align(s_list, LV_ALIGN_BOTTOM_MID, 0, -6);
    /* Without this the last row (Apps' taller switch+trash rows especially)
     * sits flush against the list's own bottom edge and reads as clipped. */
    lv_obj_set_style_pad_bottom(s_list, 8, LV_PART_MAIN);

    if (!s_group) {
        s_group = lv_group_create();
    }
    shell_set_input_group(s_group);

    show_view(VIEW_MENU);
    s_refresh_timer = lv_timer_create(refresh_tick, 2000, NULL);

    lv_screen_load(s_screen);
    ESP_LOGI(TAG, "opened");
    return ESP_OK;
}

void settings_close(void)
{
    if (!s_screen) {
        return;
    }
    /* The system "home" shortcut can fire while setup is mid-flight — must not
     * leave the device stuck in AP mode with no way back to STA. */
    if (s_wifi_panel) {
        wifi_setup_close(true);
    }
    /* Same reasoning — the escape hatch must not leave the screen showing a
     * delete-confirm panel over an s_screen that is about to be torn down. */
    if (s_delete_panel) {
        delete_panel_close();
    }
    if (s_refresh_timer) {
        lv_timer_delete(s_refresh_timer);
        s_refresh_timer = NULL;
    }
    if (s_prev_screen) {
        lv_screen_load(s_prev_screen);
    }
    shell_set_input_group(NULL);   /* back to the launcher's own group */
    lv_obj_delete(s_screen);
    s_screen = NULL;
    s_list = NULL;
    /* Every row pointer above lived inside s_list and is gone now. An OTA
     * update in particular keeps running on its own task regardless of
     * whether this screen (or the About view within it) is open — null
     * s_ota_status_label or ota_status_cb() would write through a dangling
     * pointer the next time it reports progress. */
    s_brightness_slider = NULL;
    s_brightness_editing = false;
    s_tz_btn = NULL;
    s_wifi_btn = NULL;
    s_ota_btn = NULL;
    s_ota_status_label = NULL;
    s_ram_arc = NULL;
    s_ram_pct_label = NULL;
    s_delete_trigger_btn = NULL;
}

bool settings_is_open(void)
{
    return s_screen != NULL;
}
