#include "settings.h"

#include <stdio.h>
#include <string.h>

#include "app_registry.h"
#include "bsp.h"
#include "esp_app_desc.h"
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

static lv_obj_t   *s_brightness_btn;      /* Display */
static lv_obj_t   *s_tz_btn;              /* Network */
static lv_obj_t   *s_wifi_btn;            /* Network */
static lv_obj_t   *s_ota_btn;             /* About */
static lv_obj_t   *s_ota_status_label;    /* About */
static lv_obj_t   *s_heap_label;          /* Storage */

/* WiFi setup sub-panel — swapped in over the list, torn down on cancel, on a
 * result settling, or on settings_close() (the system "home" shortcut can
 * fire mid-setup and must not strand the device in AP mode). Only ever opened
 * from the Network view. */
static lv_obj_t   *s_wifi_panel;
static lv_obj_t   *s_wifi_status_label;
static lv_timer_t *s_wifi_timer;
static uint8_t     s_wifi_settle_ticks;

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

static void brightness_label_update(void)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "Brightness: %u%%", bsp_backlight_get());
    lv_list_set_button_text(s_list, s_brightness_btn, buf);
}

static void brightness_clicked(lv_event_t *e)
{
    (void)e;
    uint8_t pct = bsp_backlight_get();
    pct = (pct >= 100) ? 20 : (uint8_t)(pct + 20);
    bsp_backlight_set(pct);
    bsp_backlight_save(pct);
    brightness_label_update();
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

static void tz_clicked(lv_event_t *e)
{
    (void)e;
    const int idx = tz_preset_index_of_current();
    const int next = (idx + 1 >= TZ_PRESET_COUNT) ? 0 : idx + 1;
    net_set_timezone(TZ_PRESETS[next].tz);
    tz_label_update();
}

static void pin_row_set_text(lv_obj_t *btn, const app_info_t *app)
{
    char buf[48];
    snprintf(buf, sizeof(buf), "%s  %s",
             settings_app_is_pinned(app->id) ? LV_SYMBOL_OK : " ", app->name);
    lv_list_set_button_text(s_list, btn, buf);
}

static void pin_clicked(lv_event_t *e)
{
    const app_info_t *app = lv_event_get_user_data(e);
    pinned_toggle(app->id);
    pin_row_set_text(lv_event_get_target(e), app);
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

static void refresh_tick(lv_timer_t *timer)
{
    (void)timer;
    if (s_heap_label) {
        char buf[32];
        snprintf(buf, sizeof(buf), "Free heap: %lu KB",
                 (unsigned long)(esp_get_free_heap_size() / 1024));
        lv_label_set_text(s_heap_label, buf);
    }
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
    lv_obj_set_size(s_wifi_panel, BSP_LCD_H_RES - 24, BSP_LCD_V_RES - 24);
    lv_obj_center(s_wifi_panel);
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

static void back_clicked(lv_event_t *e)
{
    (void)e;
    show_view(VIEW_MENU);
}

static void add_back_row(void)
{
    lv_obj_t *btn = lv_list_add_button(s_list, LV_SYMBOL_LEFT, "Back");
    lv_obj_add_event_cb(btn, back_clicked, LV_EVENT_CLICKED, NULL);
    lv_group_add_obj(s_group, btn);
}

static void category_clicked(lv_event_t *e)
{
    const settings_view_t view = (settings_view_t)(intptr_t)lv_event_get_user_data(e);
    show_view(view);
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

/* Logo placeholder — no real asset yet (see ROADMAP #28 on the lack of any
 * icon pipeline at all), just a coloured circle reserving the spot at the
 * top of the device identity screen. */
static void build_about(void)
{
    add_back_row();

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
    lv_obj_set_style_bg_color(logo, lv_color_hex(0x4A9EFF), LV_PART_MAIN);
    lv_obj_set_style_border_width(logo, 0, LV_PART_MAIN);

    const esp_app_desc_t *desc = esp_app_get_description();
    char line[64];

    snprintf(line, sizeof(line), "Device: %s", desc->project_name);
    lv_list_add_text(s_list, line);

    snprintf(line, sizeof(line), "Firmware: %s", desc->version);
    lv_list_add_text(s_list, line);

    s_ota_btn = lv_list_add_button(s_list, NULL, "Check for update");
    lv_obj_add_event_cb(s_ota_btn, ota_clicked, LV_EVENT_CLICKED, NULL);
    lv_group_add_obj(s_group, s_ota_btn);
    s_ota_status_label = lv_list_add_text(s_list, "");
}

static void build_display(void)
{
    add_back_row();

    s_brightness_btn = lv_list_add_button(s_list, NULL, "");
    lv_obj_add_event_cb(s_brightness_btn, brightness_clicked, LV_EVENT_CLICKED, NULL);
    lv_group_add_obj(s_group, s_brightness_btn);
    brightness_label_update();
}

static void build_network(void)
{
    add_back_row();

    s_tz_btn = lv_list_add_button(s_list, NULL, "");
    lv_obj_add_event_cb(s_tz_btn, tz_clicked, LV_EVENT_CLICKED, NULL);
    lv_group_add_obj(s_group, s_tz_btn);
    tz_label_update();

    s_wifi_btn = lv_list_add_button(s_list, NULL, "");
    lv_obj_add_event_cb(s_wifi_btn, wifi_clicked, LV_EVENT_CLICKED, NULL);
    lv_group_add_obj(s_group, s_wifi_btn);
    wifi_label_update();
}

static void build_storage(void)
{
    add_back_row();

    char line[64];

    lv_list_add_text(s_list, bsp_sd_is_mounted() ? "SD card: mounted" : "SD card: not present");
    if (bsp_sd_is_mounted()) {
        char name[32];
        uint32_t capacity_mb = 0;
        bsp_sd_info(name, sizeof(name), &capacity_mb);
        snprintf(line, sizeof(line), "SD: %s, %lu MB", name, (unsigned long)capacity_mb);
        lv_list_add_text(s_list, line);
    }

    size_t used_kb = 0, total_kb = 0;
    bsp_fs_usage(&used_kb, &total_kb);
    snprintf(line, sizeof(line), "Internal storage: %u/%u KB",
             (unsigned)used_kb, (unsigned)total_kb);
    lv_list_add_text(s_list, line);

    s_heap_label = lv_list_add_text(s_list, "");
}

static void build_apps(void)
{
    add_back_row();

    lv_group_t *group = s_group;
    lv_list_add_text(s_list, "Pinned on Home:");
    const size_t n = app_registry_count();
    for (size_t i = 0; i < n; i++) {
        const app_info_t *app = app_registry_get(i);
        lv_obj_t *btn = lv_list_add_button(s_list, NULL, "");
        lv_obj_add_event_cb(btn, pin_clicked, LV_EVENT_CLICKED, (void *)app);
        lv_group_add_obj(group, btn);
        pin_row_set_text(btn, app);
    }
}

static void show_view(settings_view_t view)
{
    lv_obj_clean(s_list);
    s_brightness_btn = NULL;
    s_tz_btn = NULL;
    s_wifi_btn = NULL;
    s_ota_btn = NULL;
    s_ota_status_label = NULL;
    s_heap_label = NULL;

    switch (view) {
    case VIEW_ABOUT:   build_about();   break;
    case VIEW_DISPLAY: build_display(); break;
    case VIEW_NETWORK: build_network(); break;
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
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(0x101418), LV_PART_MAIN);
    lv_obj_remove_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    s_list = lv_list_create(s_screen);
    lv_obj_set_size(s_list, BSP_LCD_H_RES - 12, BSP_LCD_V_RES - 12);
    lv_obj_center(s_list);

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
    s_brightness_btn = NULL;
    s_tz_btn = NULL;
    s_wifi_btn = NULL;
    s_ota_btn = NULL;
    s_ota_status_label = NULL;
    s_heap_label = NULL;
}

bool settings_is_open(void)
{
    return s_screen != NULL;
}
