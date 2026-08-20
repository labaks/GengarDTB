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

#ifdef __cplusplus
}
#endif
