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
#include "input.h"
#include "lvgl.h"
#include "net.h"
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
    static const char *nets[]     = { "--", "..", "ok" };

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

/* ------------------------------------------------------------ system chords */

/* Returns true when the event was a system action and must not reach the app. */
static bool handle_system_chord(const input_event_t *ev)
{
    /* B1+B3 short: home / launcher. */
    if (ev->mask == (INPUT_B1 | INPUT_B3) && ev->kind == INPUT_EV_CLICK) {
        if (widget_is_open()) {
            ESP_LOGI(TAG, "system: home (closing '%s')", widget_current()->id);
            widget_close();
        }
        return true;
    }

    /* B1+B2 (click or autorepeat): brightness. Wraps so one chord is enough. */
    if (ev->mask == (INPUT_B1 | INPUT_B2) &&
        (ev->kind == INPUT_EV_CLICK || ev->kind == INPUT_EV_HOLD_REPEAT)) {
        uint8_t pct = bsp_backlight_get();
        pct = (pct >= 100) ? 20 : (uint8_t)(pct + 20);
        bsp_backlight_set(pct);
        ESP_LOGI(TAG, "system: backlight %u%%", pct);
        return true;
    }

    /* All three, long: escape hatch. This one is non-negotiable — without it a
     * misbehaving widget can lock the user out of their own device. */
    if (ev->mask == INPUT_MASK_ALL && ev->kind == INPUT_EV_LONG_PRESS) {
        ESP_LOGW(TAG, "system: force return to launcher");
        widget_close();
        return true;
    }

    return false;
}

static uint32_t translate_key(const input_event_t *ev)
{
    if (ev->mask == INPUT_B1 &&
        (ev->kind == INPUT_EV_CLICK || ev->kind == INPUT_EV_HOLD_REPEAT)) {
        return LV_KEY_PREV;
    }
    if (ev->mask == INPUT_B2 &&
        (ev->kind == INPUT_EV_CLICK || ev->kind == INPUT_EV_HOLD_REPEAT)) {
        return LV_KEY_NEXT;
    }
    if (ev->mask == INPUT_B3 && ev->kind == INPUT_EV_CLICK) {
        return LV_KEY_ENTER;
    }
    if (ev->mask == INPUT_B3 && ev->kind == INPUT_EV_LONG_PRESS) {
        return LV_KEY_ESC;
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
        return;
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

    s_group = lv_group_create();
    lv_group_set_default(s_group);

    lv_indev_t *touch = lv_indev_create();
    lv_indev_set_type(touch, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(touch, touch_read_cb);
    lv_indev_set_display(touch, s_disp);

    lv_indev_t *keypad = lv_indev_create();
    lv_indev_set_type(keypad, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(keypad, keypad_read_cb);
    lv_indev_set_display(keypad, s_disp);
    lv_indev_set_group(keypad, s_group);

    build_launcher();
    status_refresh();

    lv_timer_create(shell_tick, 10, NULL);
    net_on_state_change(on_net_state);

    lvgl_port_unlock();

    /* First frame is drawn; safe to light the panel now. */
    bsp_backlight_set(BACKLIGHT_DEFAULT);

    ESP_LOGI(TAG, "shell up");
    return ESP_OK;
}
