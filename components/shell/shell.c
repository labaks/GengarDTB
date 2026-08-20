/*
 * deskos shell — global toolbar + Home/Full list/Settings navigation, driven
 * by the input layer. See docs/shell-navigation.md for the screen map this
 * file implements.
 *
 * Input routing, in order:
 *   1. system chords are consumed here and never reach the app
 *   2. on Home, B1/B2 cycle pinned widgets directly (no LVGL group involved)
 *   3. everywhere else, keys are translated to LVGL PREV/NEXT/ENTER and fed
 *      to whichever focus group currently owns the keypad indev
 *
 * NOTE ON TEXT: LVGL's bundled Montserrat fonts have no Cyrillic glyphs, so every
 * string here is Latin on purpose. Russian labels need a font generated with
 * lv_font_conv first — see sdkconfig.defaults.
 */
#include "shell.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "app_registry.h"
#include "bsp.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "host.h"
#include "input.h"
#include "lvgl.h"
#include "net.h"
#include "nvs.h"
#include "settings.h"
#include "widget.h"

static const char *TAG = "shell";

/* The main heap knob: 320 lines * 2 bytes each. Two buffers, so 24 lines costs
 * 30 KB. Deliberately smaller than it could be — WiFi takes roughly 50 KB and a
 * TLS handshake peaks around 40 KB more, and this board has no PSRAM. */
#define LCD_BUFFER_LINES  24
/* This panel leaks a fair amount of backlight through black pixels — at full
 * brightness a dark UI reads as washed-out blue-grey rather than black. 55%
 * keeps it legible indoors while letting the dark theme actually look dark. */
#define BACKLIGHT_DEFAULT 55

static lv_display_t *s_disp;
static lv_group_t   *s_group;      /* Full list's focus group (the base/default one) */
static lv_indev_t   *s_keypad;
static lv_obj_t     *s_status;       /* left side: clock + input backend, lv_layer_top() */
static lv_obj_t     *s_status_right; /* right side: wifi + pc, lv_layer_top() */
static bool          s_host_connected;

typedef enum {
    SHELL_HOME,
    SHELL_FULL_LIST,
} shell_mode_t;

static shell_mode_t     s_mode = SHELL_HOME;
static lv_obj_t         *s_home_screen;
static lv_obj_t         *s_home_label;
static lv_obj_t         *s_full_list_screen;
static const app_info_t *s_pinned[APP_REGISTRY_MAX];
static size_t            s_pinned_count;
static size_t            s_pinned_idx;

/* Pending LVGL keys, filled by shell_tick and drained by the keypad read callback.
 * Two-phase: LVGL wants a PRESSED sample followed by a RELEASED one, while our
 * input layer speaks in completed clicks. */
static uint32_t s_key_ring[8];
static uint8_t  s_key_head, s_key_tail;
static uint32_t s_key_active;
static bool     s_key_pressed;

static void key_push(uint32_t key)
{
    const uint8_t next = (uint8_t)((s_key_head + 1u) % 8u);
    if (next == s_key_tail) {
        ESP_LOGW(TAG, "key ring full, dropped key %lu", (unsigned long)key);
        return;
    }
    s_key_ring[s_key_head] = key;
    s_key_head = next;
}

/* ------------------------------------------------------------- input devices */

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;

    bsp_touch_state_t t = { 0 };
    bsp_touch_read(&t);

    if (t.pressed) {
        data->point.x = t.x;
        data->point.y = t.y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static void keypad_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;

    if (s_key_pressed) {
        /* Second half of the previous key: report the release and clear. */
        data->key = s_key_active;
        data->state = LV_INDEV_STATE_RELEASED;
        s_key_pressed = false;
        return;
    }

    if (s_key_tail != s_key_head) {
        s_key_active = s_key_ring[s_key_tail];
        s_key_tail = (uint8_t)((s_key_tail + 1u) % 8u);
        data->key = s_key_active;
        data->state = LV_INDEV_STATE_PRESSED;
        s_key_pressed = true;
        return;
    }

    data->state = LV_INDEV_STATE_RELEASED;
}

/* ---------------------------------------------------------------- status bar */

/* Lives on lv_layer_top(), a fixed overlay LVGL draws above whatever screen is
 * currently active via lv_screen_load() — Home, Full list, Settings, or an
 * open widget. Without this the bar used to be a plain child of the launcher
 * screen and vanished every time anything else was shown (see
 * docs/shell-navigation.md). Ticks every second so the clock keeps moving,
 * not just on WiFi/host state changes. */
/* Recolor spans (LV_SYMBOL_WIFI/LV_SYMBOL_DRIVE inline-colored via
 * lv_label_set_recolor) instead of the "wifi ok"/"pc --" text this bar used
 * to show — those words told a user nothing, an icon dimmed to grey for
 * "not connected" reads at a glance. Dim grey matches s_status's own text
 * color (0x9aa7b0) so "off" icons blend into the same tone as the clock. */
#define STATUS_DIM   "9aa7b0"
#define STATUS_UP    "4caf50"
#define STATUS_WAIT  "c9a227"
#define STATUS_AP    "4a90d9"

static void status_refresh(lv_timer_t *timer)
{
    (void)timer;

    if (!s_status || !s_status_right) {
        return;
    }

    char clock_buf[8] = "--:--";
    if (net_time_valid()) {
        const time_t now = time(NULL);
        struct tm tm_local;
        localtime_r(&now, &tm_local);
        strftime(clock_buf, sizeof(clock_buf), "%H:%M", &tm_local);
    }
    lv_label_set_text(s_status, clock_buf);

    static const char *wifi_colors[] = { STATUS_DIM, STATUS_WAIT, STATUS_UP, STATUS_AP };
    char right[64];
    snprintf(right, sizeof(right), "#%s %s# #%s %s#",
             wifi_colors[net_state()], LV_SYMBOL_WIFI,
             s_host_connected ? STATUS_UP : STATUS_DIM, LV_SYMBOL_DRIVE);
    lv_label_set_text(s_status_right, right);
}

/* Called from the WiFi event task, so it has to take the LVGL lock itself. */
static void on_net_state(net_state_t state)
{
    (void)state;
    if (lvgl_port_lock(200)) {
        status_refresh(NULL);
        lvgl_port_unlock();
    }
}

/* Called from the host client's supervisor/WS-event task — shell_set_host_connected()
 * already takes the LVGL lock itself, same as widget.c's capability check reads
 * s_host_connected without one (a plain bool, same informal convention net.c
 * uses for its own state). */
static void on_host_state(host_state_t state)
{
    shell_set_host_connected(state == HOST_UP);
}

void shell_set_host_connected(bool connected)
{
    if (s_host_connected == connected) {
        return;
    }
    s_host_connected = connected;

    if (lvgl_port_lock(100)) {
        status_refresh(NULL);
        lvgl_port_unlock();
    }
}

lv_group_t *shell_input_group(void)
{
    return s_group;
}

void shell_set_input_group(lv_group_t *group)
{
    if (s_keypad) {
        lv_indev_set_group(s_keypad, group ? group : s_group);
    }
}

/* ------------------------------------------------------------ Home / Full list */

static void app_clicked(lv_event_t *e)
{
    const app_info_t *app = lv_event_get_user_data(e);

    if (!app_registry_is_available(app, s_host_connected)) {
        /* Degraded rather than hidden: the entry stays in the list so the user
         * can see the widget exists, but opening it would show nothing useful. */
        ESP_LOGW(TAG, "'%s' is unavailable right now", app->id);
        return;
    }

    ESP_LOGI(TAG, "launching '%s' from %s", app->id, app->dir);
    const esp_err_t err = widget_open(app);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cannot open '%s': %s", app->id, esp_err_to_name(err));
    }
}

static void settings_clicked(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "opening settings");
    const esp_err_t err = settings_open();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cannot open settings: %s", esp_err_to_name(err));
    }
}

/* Home: a fullscreen render of one pinned widget at a time (reuses
 * widget_open() as-is — it does not care who opened it), or a static
 * placeholder when nothing is pinned yet. B1/B2 cycle pinned widgets
 * directly; there is no list to navigate, so this does not touch lv_group at
 * all (see shell_tick). */
static void show_home(void)
{
    s_mode = SHELL_HOME;
    lv_screen_load(s_home_screen);   /* must be active before widget_open() below,
                                       * so its own "previous screen" bookkeeping
                                       * points back here rather than Full list. */

    s_pinned_count = settings_pinned_apps(s_pinned, APP_REGISTRY_MAX);
    if (s_pinned_idx >= s_pinned_count) {
        s_pinned_idx = 0;
    }

    if (s_pinned_count == 0) {
        lv_label_set_text(s_home_label, "No pinned widgets.\nOpen Full list to pin one.");
        return;
    }
    lv_label_set_text(s_home_label, "");
    widget_open(s_pinned[s_pinned_idx]);
}

static void home_step(int dir)
{
    if (s_pinned_count == 0) {
        return;
    }
    s_pinned_idx = (size_t)(((int)s_pinned_idx + dir + (int)s_pinned_count) % (int)s_pinned_count);
    widget_open(s_pinned[s_pinned_idx]);   /* closes whatever pinned widget was showing first */
}

static void show_full_list(void)
{
    if (widget_is_open()) {
        widget_close();
    }
    s_mode = SHELL_FULL_LIST;
    lv_screen_load(s_full_list_screen);
}

static void build_home_screen(void)
{
    s_home_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_home_screen, lv_color_hex(0x101418), LV_PART_MAIN);
    lv_obj_remove_flag(s_home_screen, LV_OBJ_FLAG_SCROLLABLE);

    s_home_label = lv_label_create(s_home_screen);
    lv_obj_set_style_text_color(s_home_label, lv_color_hex(0xC0C8D0), LV_PART_MAIN);
    lv_label_set_long_mode(s_home_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_home_label, LV_PCT(80));
    lv_obj_set_style_text_align(s_home_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_center(s_home_label);
}

/* A grid of tiles instead of the old vertical list — same primitives
 * (lv_button + lv_label), just wrapped instead of stacked. No icons: there is
 * no image asset pipeline yet (ROADMAP #28), so a tile is its app name. */
static void build_full_list_screen(void)
{
    s_full_list_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_full_list_screen, lv_color_hex(0x101418), LV_PART_MAIN);
    lv_obj_set_flex_flow(s_full_list_screen, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_all(s_full_list_screen, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_top(s_full_list_screen, SHELL_TOOLBAR_HEIGHT + 4, LV_PART_MAIN);   /* room for the toolbar */
    lv_obj_set_style_pad_row(s_full_list_screen, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_column(s_full_list_screen, 8, LV_PART_MAIN);

    const size_t n = app_registry_count();
    if (n == 0) {
        lv_obj_t *empty = lv_label_create(s_full_list_screen);
        lv_label_set_text(empty,
                          bsp_sd_is_mounted() ? "No apps in /sd/apps"
                                              : "Insert microSD card");
    }

    for (size_t i = 0; i < n; i++) {
        const app_info_t *app = app_registry_get(i);

        char label[64];
        if (app_registry_is_available(app, s_host_connected)) {
            snprintf(label, sizeof(label), "%s", app->name);
        } else {
            /* Degraded, not hidden: the user should see the widget exists and why
             * it cannot run, rather than wondering where it went. */
            snprintf(label, sizeof(label), "%s\n(unavailable)", app->name);
        }

        lv_obj_t *tile = lv_button_create(s_full_list_screen);
        lv_obj_set_size(tile, 92, 64);
        lv_obj_t *lbl = lv_label_create(tile);
        lv_obj_set_width(lbl, LV_PCT(100));
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_label_set_text(lbl, label);
        lv_obj_center(lbl);

        lv_obj_add_event_cb(tile, app_clicked, LV_EVENT_CLICKED, (void *)app);
        lv_group_add_obj(s_group, tile);
    }

    /* Settings is not from the registry — a fixed tile the shell always adds
     * itself (see docs/shell-navigation.md). Always present, even with zero
     * apps/no card, so it stays reachable in every state. */
    lv_obj_t *settings_tile = lv_button_create(s_full_list_screen);
    lv_obj_set_size(settings_tile, 92, 64);
    lv_obj_t *settings_lbl = lv_label_create(settings_tile);
    lv_label_set_text(settings_lbl, "Settings");
    lv_obj_center(settings_lbl);
    lv_obj_add_event_cb(settings_tile, settings_clicked, LV_EVENT_CLICKED, NULL);
    lv_group_add_obj(s_group, settings_tile);
}

/* ------------------------------------------------------------ system chords */

/* The two layouts differ by more than one binding, so the mapping is chosen from
 * how many buttons are actually wired rather than hardcoded. With only B1 and B2
 * there is no dedicated select key, so B1+B2 becomes ENTER — which in turn
 * displaces the escape hatch onto a long press. */
static bool three_buttons(void)
{
    return (input_get_present_mask() & INPUT_B3) != 0;
}

static bool is_enter(const input_event_t *ev)
{
    const bool click = ev->kind == INPUT_EV_CLICK;
    return three_buttons() ? (ev->mask == INPUT_B3 && click)
                            : (ev->mask == (INPUT_B1 | INPUT_B2) && click);
}

/* "Home" is always reachable and always lands on the same place — there is no
 * back-stack (see docs/shell-navigation.md): a widget opened from Full list
 * still returns to Home, not to Full list, same as a widget pinned to Home
 * itself. */
static void go_home(const char *why)
{
    /* A widget being open is NOT "elsewhere" by itself — Home shows one
     * whenever something is pinned. Only Full list (browsing tiles, or
     * drilled into a widget/Settings from there) counts. */
    if (s_mode != SHELL_FULL_LIST) {
        return;   /* already home, nothing to do */
    }

    if (widget_is_open()) {
        ESP_LOGI(TAG, "system: home (%s), closing '%s'", why, widget_current()->id);
        widget_close();
    }
    if (settings_is_open()) {
        ESP_LOGI(TAG, "system: home (%s), closing settings", why);
        settings_close();
    }
    show_home();
}

/* Returns true when the event was a system action and must not reach the app. */
static bool handle_system_chord(const input_event_t *ev)
{
    const bool longp  = ev->kind == INPUT_EV_LONG_PRESS;
    const bool click  = ev->kind == INPUT_EV_CLICK;

    /* B1 long is home under BOTH layouts, and is the only route back that a
     * resistive panel can produce — it is single-touch, so no chord is
     * reachable by finger. Without it a touch-only user who opens a widget is
     * stuck until a reset. */
    if (ev->mask == INPUT_B1 && longp) {
        go_home("long press");
        return true;
    }

    /* Home -> Full list, the one system-level action that is NOT unconditional:
     * it only means something while sitting on Home. Elsewhere the same chord
     * is ENTER (a Full list tile) or an app's own manifest binding — "system
     * always wins" only applies to chords the system actually claims right now. */
    if (s_mode == SHELL_HOME && !settings_is_open() && is_enter(ev)) {
        ESP_LOGI(TAG, "system: home -> full list");
        show_full_list();
        return true;
    }

    if (three_buttons()) {
        if (ev->mask == (INPUT_B1 | INPUT_B3) && click) {
            go_home("chord");
            return true;
        }
        /* Escape hatch. Non-negotiable: without it a misbehaving widget can
         * lock the user out of their own device. */
        if (ev->mask == INPUT_MASK_ALL && longp) {
            ESP_LOGW(TAG, "system: force return to launcher");
            widget_close();
            settings_close();
            show_home();
            return true;
        }
    } else {
        if (ev->mask == (INPUT_B1 | INPUT_B2) && longp) {
            ESP_LOGW(TAG, "system: force return to launcher");
            widget_close();
            settings_close();
            show_home();
            return true;
        }
    }

    return false;
}

static uint32_t translate_key(const input_event_t *ev)
{
    const bool click  = ev->kind == INPUT_EV_CLICK;
    const bool repeat = ev->kind == INPUT_EV_HOLD_REPEAT;

    if (ev->mask == INPUT_B1 && (click || repeat)) {
        return LV_KEY_PREV;
    }
    if (ev->mask == INPUT_B2 && (click || repeat)) {
        return LV_KEY_NEXT;
    }

    if (three_buttons()) {
        if (ev->mask == INPUT_B3 && click) {
            return LV_KEY_ENTER;
        }
        if (ev->mask == INPUT_B3 && ev->kind == INPUT_EV_LONG_PRESS) {
            return LV_KEY_ESC;
        }
    } else if (ev->mask == (INPUT_B1 | INPUT_B2) && click) {
        return LV_KEY_ENTER;
    }

    return 0;
}

/* Runs in the LVGL task, so it may touch LVGL objects directly. */
static void shell_tick(lv_timer_t *timer)
{
    (void)timer;

    input_event_t ev;
    while (input_get_event(&ev, 0)) {
        /* Logged at INFO on purpose while the buttons are unsoldered: this line is
         * how injected chords get verified against the decoder. */
        ESP_LOGI(TAG, "input: %s", input_event_str(&ev));

        if (handle_system_chord(&ev)) {
            status_refresh(NULL);
            continue;
        }

        /* Home has no list to navigate — B1/B2 step through pinned widgets
         * directly instead of driving a focus group. Only reachable here when
         * is_enter() above did not already consume the event as "go to Full
         * list", so a pinned widget's own manifest binding on the same chord
         * (e.g. weather's refresh) never fires while it is showing on Home —
         * system navigation wins, same rule as everywhere else.
         *
         * CLICK only, not HOLD_REPEAT: found on hardware that holding a
         * button (e.g. B1 for the "go home" long-press) keeps generating
         * repeat events for that same bare mask for as long as it stays
         * down. Reacting to those here span-cycled through pinned widgets
         * (each with its own HTTP refetch) for the entire duration of an
         * unrelated long-press. Plain click has no such follow-on. */
        if (s_mode == SHELL_HOME) {
            const bool click = ev.kind == INPUT_EV_CLICK;
            if (ev.mask == INPUT_B1 && click) {
                home_step(-1);
                continue;
            }
            if (ev.mask == INPUT_B2 && click) {
                home_step(+1);
                continue;
            }
        }

        /* Settings' inline brightness slider behaves like an encoder-edited
         * menu item: selecting it (ENTER, unaffected by anything here) toggles
         * "editing" in settings.c, and while editing, B1/B2 step its value
         * instead of moving list focus. CLICK only, not HOLD_REPEAT: a bare
         * B1 hold is unconditionally "go home" (fires above, on the
         * LONG_PRESS that precedes any repeat), so holding it to scrub was
         * never reachable anyway — click-to-step keeps both buttons
         * symmetric. See settings_is_adjusting_brightness()'s own comment for
         * why this has to be raw shell_tick routing rather than lv_group. */
        if (settings_is_adjusting_brightness()) {
            const bool click = ev.kind == INPUT_EV_CLICK;
            if (ev.mask == INPUT_B1 && click) {
                settings_brightness_step(-5);
                continue;
            }
            if (ev.mask == INPUT_B2 && click) {
                settings_brightness_step(+5);
                continue;
            }
        }

        /* System chords always get first refusal (above) — a manifest
         * binding that collides with one simply never reaches this check,
         * which is the whole conflict-resolution rule from ROADMAP #16. */
        if (widget_is_open() &&
            app_registry_action_for(widget_current(), &ev) == APP_ACTION_REFRESH) {
            ESP_LOGI(TAG, "app binding: refresh '%s'", widget_current()->id);
            widget_refresh_now();
            continue;
        }

        const uint32_t key = translate_key(&ev);
        if (key) {
            key_push(key);
        }
    }
}

/* ------------------------------------------------------- touch calibration */

/* Inset from the physical edge: resistive panels are hard to hit exactly at
 * pixel 0, and a target placed here still calibrates the true edges — a touch
 * right at the target maps to screen 0/max, everything beyond just clamps.
 *
 * Touches near the top edge are measurably noisier/less repeatable than the
 * rest of the panel on this unit (worse with a stylus than a finger). Tried
 * widening this to 40px on the theory that the inset targets themselves sat
 * in the noisiest part of the panel — made no difference, so reverted. The
 * instability is not fixable by where the calibration targets sit; see
 * CLAUDE.md for the current state of that investigation. */
#define CAL_INSET 24

typedef struct { int16_t x, y; } cal_point_t;

static void cal_wait_for_release(void)
{
    uint16_t x, y, z;
    while (bsp_touch_read_raw(&x, &y, &z)) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static uint16_t median_of(uint16_t *v, int n)
{
    /* n is at most CAL_SAMPLES (32) — insertion sort is plenty. */
    for (int i = 1; i < n; i++) {
        const uint16_t key = v[i];
        int j = i - 1;
        while (j >= 0 && v[j] > key) {
            v[j + 1] = v[j];
            j--;
        }
        v[j + 1] = key;
    }
    return v[n / 2];
}

#define CAL_SAMPLES     32
#define CAL_MIN_SAMPLES 10   /* ~200ms held — a shorter tap/bounce is rejected
                               * rather than trusted; two calibration runs
                               * gave meaningfully different Y bounds (span
                               * 3081 vs. 1612), and the runs with fewer
                               * samples per point were the noisier ones. */

/* The very first sample right as contact begins can catch a transient rather
 * than a settled reading (this is what burned the first calibration attempt:
 * saved bounds a real tap could never reproduce). Sampling for the whole hold
 * and taking the median is what "touch and hold the target" actually needs
 * to mean for this to work. Returns false on a too-brief touch; the caller
 * retries silently rather than calibrating off an unreliable capture. */
static bool cal_capture_one(uint16_t *out_rx, uint16_t *out_ry)
{
    uint16_t x, y, z;
    while (!bsp_touch_read_raw(&x, &y, &z)) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    uint16_t xs[CAL_SAMPLES], ys[CAL_SAMPLES];
    int n = 0;
    do {
        if (n < CAL_SAMPLES) {
            xs[n] = x;
            ys[n] = y;
            n++;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    } while (bsp_touch_read_raw(&x, &y, &z) && n < CAL_SAMPLES);

    if (n < CAL_MIN_SAMPLES) {
        return false;
    }
    *out_rx = median_of(xs, n);
    *out_ry = median_of(ys, n);
    return true;
}

static void cal_wait_for_press(uint16_t *out_rx, uint16_t *out_ry)
{
    while (!cal_capture_one(out_rx, out_ry)) {
        /* too brief to trust — try again, same target */
    }
}

/* Runs before indevs/screens exist, so it polls the raw touch reading
 * directly (see bsp.h) rather than going through an LVGL indev. Targets are
 * added to whatever screen is already active (LVGL's own initial default
 * screen) and removed again afterwards — that screen is then abandoned once
 * show_home() loads the real Home screen right after this returns. */
static void run_touch_calibration(void)
{
    static const cal_point_t targets[4] = {
        { CAL_INSET,                     CAL_INSET },
        { BSP_LCD_H_RES - 1 - CAL_INSET, CAL_INSET },
        { CAL_INSET,                     BSP_LCD_V_RES - 1 - CAL_INSET },
        { BSP_LCD_H_RES - 1 - CAL_INSET, BSP_LCD_V_RES - 1 - CAL_INSET },
    };
    uint16_t rx[4], ry[4];

    lvgl_port_lock(0);
    lv_obj_t *label = lv_label_create(lv_screen_active());
    lv_obj_set_style_text_color(label, lv_color_hex(0xC0C8D0), LV_PART_MAIN);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *dot = lv_obj_create(lv_screen_active());
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(dot, 20, 20);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(dot, lv_color_hex(0x4A9EFF), LV_PART_MAIN);
    lv_obj_set_style_border_width(dot, 0, LV_PART_MAIN);
    lvgl_port_unlock();

    /* A finger already down from handling the board must not count. */
    cal_wait_for_release();

    for (int i = 0; i < 4; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "touch + hold %d/4", i + 1);

        lvgl_port_lock(0);
        lv_label_set_text(label, buf);
        lv_obj_set_pos(dot, targets[i].x - 10, targets[i].y - 10);
        lvgl_port_unlock();

        cal_wait_for_press(&rx[i], &ry[i]);
        cal_wait_for_release();
    }

    lvgl_port_lock(0);
    lv_obj_delete(label);
    lv_obj_delete(dot);
    lvgl_port_unlock();

    /* targets[] order is TL, TR, BL, BR. bsp_touch_read() maps raw Y to
     * screen X and raw X to screen Y (inverted) — see bsp_touch.c — so the
     * left/right pair calibrates Y (lo/hi = reading at screen 0/max) and the
     * top/bottom pair calibrates X the same way. Whichever pair comes out
     * numerically reversed is a real, valid axis polarity — not an error. */
    const uint16_t y_lo = (uint16_t)((ry[0] + ry[2]) / 2);   /* left:  TL, BL */
    const uint16_t y_hi = (uint16_t)((ry[1] + ry[3]) / 2);   /* right: TR, BR */
    const uint16_t x_hi = (uint16_t)((rx[0] + rx[1]) / 2);   /* top:   TL, TR */
    const uint16_t x_lo = (uint16_t)((rx[2] + rx[3]) / 2);   /* bottom:BL, BR */

    const esp_err_t err = bsp_touch_save_calibration(x_lo, x_hi, y_lo, y_hi);
    ESP_LOGI(TAG, "touch calibration saved: x[%u,%u] y[%u,%u] (%s)",
             x_lo, x_hi, y_lo, y_hi, esp_err_to_name(err));
}

/* --------------------------------------------------------------------- setup */

esp_err_t shell_start(esp_lcd_panel_io_handle_t io, esp_lcd_panel_handle_t panel)
{
    const lvgl_port_cfg_t port_cfg = {
        .task_priority   = 4,
        .task_stack      = 6144,
        /* Core 1. WiFi and TLS own core 0; with no PSRAM this separation is the
         * difference between a smooth UI and visible stutter on every fetch. */
        .task_affinity   = 1,
        .task_max_sleep_ms = 500,
        .timer_period_ms = 5,
    };
    esp_err_t err = lvgl_port_init(&port_cfg);
    if (err != ESP_OK) {
        return err;
    }

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle     = io,
        .panel_handle  = panel,
        .buffer_size   = BSP_LCD_H_RES * LCD_BUFFER_LINES,
        .double_buffer = true,
        .hres          = BSP_LCD_H_RES,
        .vres          = BSP_LCD_V_RES,
        .monochrome    = false,
        /* These MUST match what bsp_display_init(landscape=true) applies.
         *
         * lvgl_port_add_disp calls esp_lcd_panel_swap_xy/mirror itself with the
         * values below, overwriting whatever the BSP set. Leaving them at the
         * default false put the panel back into 240x320 portrait while LVGL
         * still believed it was 320x240 — the UI came out rotated 90 degrees
         * with stale pixels in the uncovered region.
         *
         * Setting the same rotation in both layers is safe: these are absolute
         * setters, not relative rotations, so applying swap_xy=true twice still
         * means swap_xy=true. */
        .rotation = {
            .swap_xy  = true,
            .mirror_x = true,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma = true,
            /* LVGL lays RGB565 out as native little-endian uint16, the panel
             * wants the high byte first. Verified on the bench: without this
             * the colours are wrong in a way that greys hide almost completely,
             * which is why the first symptom read as "washed out" rather than
             * "wrong colour". */
            .swap_bytes = true,
        },
    };
    s_disp = lvgl_port_add_disp(&disp_cfg);
    if (!s_disp) {
        return ESP_FAIL;
    }

    if (!lvgl_port_lock(0)) {
        return ESP_FAIL;
    }

    /* Paint the whole screen once before anything else. The panel keeps its GRAM
     * across a reset, so without this the areas LVGL has not invalidated still
     * show the previous firmware's pixels. */
    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(0x101418), LV_PART_MAIN);
    lv_obj_invalidate(lv_screen_active());
    lvgl_port_unlock();

    /* First frame is drawn; safe to light the panel now — the calibration
     * screen right below needs it on to be of any use. */
    bsp_backlight_set(bsp_backlight_load(BACKLIGHT_DEFAULT));

    if (bsp_touch_load_calibration() == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "no touch calibration in NVS, running it now");
        run_touch_calibration();
    }

    if (!lvgl_port_lock(0)) {
        return ESP_FAIL;
    }

    s_group = lv_group_create();
    lv_group_set_default(s_group);

    lv_indev_t *touch = lv_indev_create();
    lv_indev_set_type(touch, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(touch, touch_read_cb);
    lv_indev_set_display(touch, s_disp);

    s_keypad = lv_indev_create();
    lv_indev_set_type(s_keypad, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(s_keypad, keypad_read_cb);
    lv_indev_set_display(s_keypad, s_disp);
    lv_indev_set_group(s_keypad, s_group);

    /* Global toolbar, on top of every screen (see status_refresh()'s own
     * comment). An opaque bar, not just floating text — without a background
     * of its own the status text visually merged with whatever a widget or
     * screen drew directly underneath it (found on hardware: clock/weather's
     * own header text sat right where the toolbar text was). Not part of any
     * focus group and not clickable — a status readout, never an input target. */
    lv_obj_t *toolbar = lv_obj_create(lv_layer_top());
    lv_obj_remove_flag(toolbar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(toolbar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(toolbar, 0, 0);
    lv_obj_set_size(toolbar, BSP_LCD_H_RES, SHELL_TOOLBAR_HEIGHT);
    lv_obj_set_style_bg_color(toolbar, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(toolbar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(toolbar, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(toolbar, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(toolbar, 0, LV_PART_MAIN);

    s_status = lv_label_create(toolbar);
    lv_obj_set_style_text_color(s_status, lv_color_hex(0x9aa7b0), LV_PART_MAIN);
    lv_obj_align(s_status, LV_ALIGN_LEFT_MID, 6, 0);
    lv_obj_remove_flag(s_status, LV_OBJ_FLAG_CLICKABLE);

    s_status_right = lv_label_create(toolbar);
    lv_label_set_recolor(s_status_right, true);
    lv_obj_align(s_status_right, LV_ALIGN_RIGHT_MID, -6, 0);
    lv_obj_remove_flag(s_status_right, LV_OBJ_FLAG_CLICKABLE);

    build_home_screen();
    build_full_list_screen();
    show_home();
    status_refresh(NULL);

    lv_timer_create(shell_tick, 10, NULL);
    lv_timer_create(status_refresh, 1000, NULL);
    net_on_state_change(on_net_state);
    host_on_state_change(on_host_state);

    lvgl_port_unlock();

    ESP_LOGI(TAG, "shell up");
    return ESP_OK;
}
