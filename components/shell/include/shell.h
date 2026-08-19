#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "esp_lcd_types.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Brings up LVGL on the given panel, registers the touch and keypad input devices,
 * and draws the launcher. Takes ownership of neither handle. */
esp_err_t shell_start(esp_lcd_panel_io_handle_t io, esp_lcd_panel_handle_t panel);

/* Connection state of the PC agent. Widgets declaring APP_CAP_HOST are shown as
 * degraded rather than hidden when this is false — the device must stay useful
 * while the PC is asleep. */
void shell_set_host_connected(bool connected);

/* The single LVGL input group that B1/B2/B1+B2 drive (PREV/NEXT/ENTER). Any
 * screen built outside shell.c (settings.c today) adds its focusable objects
 * here to get button navigation for free — same group, same key mapping,
 * regardless of which screen currently owns it. */
lv_group_t *shell_input_group(void);

#ifdef __cplusplus
}
#endif
