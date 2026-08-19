/*
 * deskos shell — status bar + launcher, driven by the input layer.
 *
 * Input routing, in order:
 *   1. system chords are consumed here and never reach the app
 *   2. everything else is translated to LVGL keys and fed to the focus group,
 *      so widgets get normal LVGL navigation for free
 *
 * NOTE ON TEXT: LVGL's bundled Montserrat fonts have no Cyrillic glyphs, so every
 * string here is Latin on purpose. Russian labels need a font generated with
 * lv_font_conv first — see sdkconfig.defaults.
 */
#include "shell.h"

#include <stdio.h>

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
static lv_group_t   *s_group;
static lv_indev_t   *s_keypad;
static lv_obj_t     *s_status;
static lv_obj_t     *s_list;
static bool          s_host_connected;

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

static void status_refresh(void)
{
    static const char *backends[] = { "gpio", "touch", "inject" };
    static const char *nets[]     = { "--", "..", "ok", "ap" };

    if (!s_status) {
        return;
    }

    char buf[80];
    snprintf(buf, sizeof(buf), "%s | wifi %s | sd %s | pc %s",
             backends[input_get_backend()],
             nets[net_state()],
             bsp_sd_is_mounted() ? "ok" : "--",
             s_host_connected ? "ok" : "--");
    lv_label_set_text(s_status, buf);
}

/* Called from the WiFi event task, so it has to take the LVGL lock itself. */
static void on_net_state(net_state_t state)
{
    (void)state;
    if (lvgl_port_lock(200)) {
        status_refresh();
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
        status_refresh();
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

/* ------------------------------------------------------------ system chords */

/* The two layouts differ by more than one binding, so the mapping is chosen from
 * how many buttons are actually wired rather than hardcoded. With only B1 and B2
 * there is no dedicated select key, so B1+B2 becomes ENTER — which in turn
 * displaces brightness and the escape hatch onto long presses. */
static bool three_buttons(void)
{
    return (input_get_present_mask() & INPUT_B3) != 0;
}

static void step_backlight(void)
{
    uint8_t pct = bsp_backlight_get();
    pct = (pct >= 100) ? 20 : (uint8_t)(pct + 20);
    bsp_backlight_set(pct);
    bsp_backlight_save(pct);
    ESP_LOGI(TAG, "system: backlight %u%%", pct);
}

static void go_home(const char *why)
{
    if (widget_is_open()) {
        ESP_LOGI(TAG, "system: home (%s), closing '%s'", why, widget_current()->id);
        widget_close();
    }
    if (settings_is_open()) {
        ESP_LOGI(TAG, "system: home (%s), closing settings", why);
        settings_close();
    }
}

/* Returns true when the event was a system action and must not reach the app. */
static bool handle_system_chord(const input_event_t *ev)
{
    const bool longp  = ev->kind == INPUT_EV_LONG_PRESS;
    const bool click  = ev->kind == INPUT_EV_CLICK;
    const bool repeat = ev->kind == INPUT_EV_HOLD_REPEAT;

    /* B1 long is home under BOTH layouts, and is the only route back that a
     * resistive panel can produce — it is single-touch, so no chord is
     * reachable by finger. Without it a touch-only user who opens a widget is
     * stuck until a reset. */
    if (ev->mask == INPUT_B1 && longp) {
        go_home("long press");
        return true;
    }

    if (three_buttons()) {
        if (ev->mask == (INPUT_B1 | INPUT_B3) && click) {
            go_home("chord");
            return true;
        }
        if (ev->mask == (INPUT_B1 | INPUT_B2) && (click || repeat)) {
            step_backlight();
            return true;
        }
        /* Escape hatch. Non-negotiable: without it a misbehaving widget can
         * lock the user out of their own device. */
        if (ev->mask == INPUT_MASK_ALL && longp) {
            ESP_LOGW(TAG, "system: force return to launcher");
            widget_close();
            settings_close();
            return true;
        }
    } else {
        if (ev->mask == INPUT_B2 && longp) {
            step_backlight();
            return true;
        }
        if (ev->mask == (INPUT_B1 | INPUT_B2) && longp) {
            ESP_LOGW(TAG, "system: force return to launcher");
            widget_close();
            settings_close();
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
            status_refresh();
            continue;
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

/* ------------------------------------------------------------------ launcher */

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

static void build_launcher(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), LV_PART_MAIN);

    s_status = lv_label_create(scr);
    lv_obj_set_style_text_color(s_status, lv_color_hex(0x7f8c8d), LV_PART_MAIN);
    lv_obj_align(s_status, LV_ALIGN_TOP_LEFT, 6, 4);

    s_list = lv_list_create(scr);
    lv_obj_set_size(s_list, BSP_LCD_H_RES - 12, BSP_LCD_V_RES - 34);
    lv_obj_align(s_list, LV_ALIGN_BOTTOM_MID, 0, -6);

    const size_t n = app_registry_count();
    if (n == 0) {
        lv_obj_t *empty = lv_label_create(s_list);
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
            snprintf(label, sizeof(label), "%s  (unavailable)", app->name);
        }

        lv_obj_t *btn = lv_list_add_button(s_list, NULL, label);
        lv_obj_add_event_cb(btn, app_clicked, LV_EVENT_CLICKED, (void *)app);
        lv_group_add_obj(s_group, btn);
    }

    /* Settings is not from the registry — a fixed entry the shell always
     * adds itself, same list, no separate system combo (see
     * docs/shell-navigation.md). Always present, even with zero apps/no card,
     * so it stays reachable in every state. */
    lv_obj_t *settings_btn = lv_list_add_button(s_list, NULL, "Settings");
    lv_obj_add_event_cb(settings_btn, settings_clicked, LV_EVENT_CLICKED, NULL);
    lv_group_add_obj(s_group, settings_btn);
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

/* Runs before indevs/launcher exist, so it polls the raw touch reading
 * directly (see bsp.h) rather than going through an LVGL indev. Targets are
 * added to whatever screen is already active and removed again afterwards —
 * build_launcher() draws onto that same screen right after this returns. */
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

    build_launcher();
    status_refresh();

    lv_timer_create(shell_tick, 10, NULL);
    net_on_state_change(on_net_state);
    host_on_state_change(on_host_state);

    lvgl_port_unlock();

    ESP_LOGI(TAG, "shell up");
    return ESP_OK;
}
