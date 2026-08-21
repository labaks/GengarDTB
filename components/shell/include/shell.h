#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "esp_lcd_types.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Height, in px, of the global toolbar (clock + status icons) on lv_layer_top().
 * It is opaque, so anything another screen draws underneath is fully hidden —
 * every screen built outside shell.c must reserve this much at the top of its
 * own layout. widget.c's own parse-error banner already sits at this exact
 * offset; the four builtin ui.jsonl layouts start their own content here too. */
#define SHELL_TOOLBAR_HEIGHT 18

/* Brings up LVGL on the given panel, registers the touch and keypad input
 * devices, and shows Home. Takes ownership of neither handle. */
esp_err_t shell_start(esp_lcd_panel_io_handle_t io, esp_lcd_panel_handle_t panel);

/* Connection state of the PC agent. Widgets declaring APP_CAP_HOST are shown as
 * degraded rather than hidden when this is false — the device must stay useful
 * while the PC is asleep. */
void shell_set_host_connected(bool connected);

/* Full list's LVGL input group (the base/default one). Do not add another
 * screen's objects here — they would stay registered (and get cycled through
 * by PREV/NEXT) even after that screen is gone. A screen built outside
 * shell.c (settings.c today) should keep its own group and swap it in via
 * shell_set_input_group() instead. */
lv_group_t *shell_input_group(void);

/* Points the keypad indev (B1/B2/B1+B2 -> PREV/NEXT/ENTER) at a different
 * group, so a screen with its own group gets the same button navigation for
 * free without sharing objects with Full list underneath it. Pass NULL to
 * restore Full list's own group. */
void shell_set_input_group(lv_group_t *group);

/* Rebuilds Full list's tiles from the current app registry contents. Call
 * after app_registry_scan() (a manual rescan or a delete, ROADMAP #18) so a
 * change on the SD card shows up without a reboot. Safe to call whether or
 * not Full list is the screen currently on display. */
void shell_refresh_app_list(void);

/* ROADMAP #32: shell-wide light/dark theme. The persisted choice lives in
 * settings.c (settings_theme_is_dark()); this pair is what actually applies
 * it to what is on screen right now:
 *
 *   - shell_apply_theme() re-points LVGL's own default theme (buttons,
 *     switches, sliders, lists, the spinner...) at the new palette via
 *     lv_theme_default_init() — every default-styled object already on
 *     screen picks this up on its own (LVGL re-applies the shared style
 *     structs in place), so Full list's tiles and every row in Settings
 *     need no code of their own to follow. It then re-colors the handful of
 *     screens that set their own background explicitly (Home, Full list) —
 *     a local style override always wins over the theme, so those never
 *     follow it automatically.
 *   - shell_theme_bg()/shell_theme_text() are that same pair of colors, for
 *     a native screen elsewhere (Settings) that sets its own root background
 *     the same way and needs to match.
 *
 * Deliberately NOT touched by either: the toolbar (always dark — it overlays
 * an open widget far more often than it overlays Home/Full list, and a
 * widget's own ui.jsonl colors never follow this theme either, see below)
 * and any open widget (ROADMAP #32's scope decision: the toggle covers the
 * shell's own chrome, not the declarative layer-A format — widget.c already
 * hardcodes its own colors per ui.jsonl line, same as it did before this
 * setting existed). */
void shell_apply_theme(bool dark);
lv_color_t shell_theme_bg(void);
lv_color_t shell_theme_text(void);

#ifdef __cplusplus
}
#endif
