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

/* The launcher's LVGL input group. Do not add another screen's objects here —
 * they would stay registered (and get cycled through by PREV/NEXT) even after
 * that screen is gone. A screen built outside shell.c (settings.c today) should
 * keep its own group and swap it in via shell_set_input_group() instead. */
lv_group_t *shell_input_group(void);

/* Points the keypad indev (B1/B2/B1+B2 -> PREV/NEXT/ENTER) at a different
 * group, so a screen with its own group gets the same button navigation for
 * free without sharing objects with the launcher underneath it. Pass NULL to
 * restore the launcher's own group. */
void shell_set_input_group(lv_group_t *group);

#ifdef __cplusplus
}
#endif
