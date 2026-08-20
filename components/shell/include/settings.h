/*
 * Native settings screen — not a ui.jsonl widget. Brightness, timezone, WiFi/SD
 * status, free heap, firmware version, and the checklist of apps pinned to
 * Home (see docs/shell-navigation.md — pinning only happens here, not via a
 * button chord on the app list).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "app_registry.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Builds and shows the settings screen. Must be called with the LVGL lock
 * held (same convention as widget_open). A no-op if already open. */
esp_err_t settings_open(void);

/* Restores the previous screen and frees everything. Safe to call when
 * nothing is open. Must be called with the LVGL lock held. */
void settings_close(void);

bool settings_is_open(void);

/* True while the Display brightness row is toggled into "editing" (selected
 * once, like any other menu row, then B1/B2 step its value instead of
 * moving focus — see brightness_slider_clicked() in settings.c). shell_tick()
 * checks this to route raw B1/B2 clicks to settings_brightness_step()
 * instead of the usual LV_KEY_PREV/NEXT: a KEYPAD-type indev never forwards
 * those two keys to a focused widget (see lv_indev.c), so there is no other
 * way to make a slider adjustable by button at all. */
bool settings_is_adjusting_brightness(void);

/* Steps the backlight by delta (clamped), commits to NVS immediately (each
 * call is one deliberate button click, not a continuous drag), and updates
 * the slider to match. A no-op unless the brightness row is in "editing". */
void settings_brightness_step(int delta);

/* True when app_id is in the pinned-to-Home set. Reads NVS directly, so it
 * is safe to call whether or not the settings screen is currently open —
 * Home calls this to pick what to show. */
bool settings_app_is_pinned(const char *app_id);

/* Pinned apps, in the order they were pinned (not registry order), for Home
 * to cycle through. Returns the count written into out (capped at max). */
size_t settings_pinned_apps(const app_info_t **out, size_t max);

/* True when showcase mode (ROADMAP #19 — auto-advance through pinned widgets
 * on Home) is on. Reads NVS directly, same as settings_app_is_pinned() —
 * shell.c's showcase_tick() polls this once a second. */
bool settings_showcase_enabled(void);

#ifdef __cplusplus
}
#endif
