/* deskos board support package — public surface.
 * Everything hardware-specific lives behind this header so a second target
 * (e.g. the ESP32-C3 satellite) only needs a new bsp implementation. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_lcd_types.h"
#include "bsp_pins.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- Display ---------------- */

/* Brings up SPI2, the ILI9341 panel and the LEDC backlight channel.
 * Hands back the esp_lcd handles so the caller can attach LVGL to them. */
esp_err_t bsp_display_init(esp_lcd_panel_io_handle_t *out_io,
                           esp_lcd_panel_handle_t *out_panel);

/* 0..100. Perceptually non-linear (raw LEDC duty); good enough for a menu slider. */
esp_err_t bsp_backlight_set(uint8_t percent);
uint8_t   bsp_backlight_get(void);

/* ---------------- Touch (XPT2046, software SPI) ---------------- */

typedef struct {
    int16_t x;          /* screen coordinates, already rotated to landscape */
    int16_t y;
    bool    pressed;
} bsp_touch_state_t;

esp_err_t bsp_touch_init(void);

/* Non-blocking. Returns the debounced current state; safe to poll from the LVGL task. */
void bsp_touch_read(bsp_touch_state_t *out);

/* Raw ADC readings, for building a calibration screen. */
bool bsp_touch_read_raw(uint16_t *out_x, uint16_t *out_y, uint16_t *out_z);

/* Calibration bounds. Defaults are typical XPT2046 values and WILL be slightly off
 * on any given panel — persist real ones once the calibration app exists. */
void bsp_touch_set_calibration(uint16_t x_min, uint16_t x_max,
                               uint16_t y_min, uint16_t y_max);

/* ---------------- Storage ---------------- */

/* LittleFS on the internal "storage" partition -> /fs. Config, cache, bindings. */
esp_err_t bsp_fs_mount(void);

/* microSD over SPI3 -> /sd. Widget apps, fonts and images live here.
 * Returns ESP_ERR_NOT_FOUND when no card is inserted — that is a normal,
 * non-fatal state: the shell must still boot and say "no card". */
esp_err_t bsp_sd_mount(void);
bool      bsp_sd_is_mounted(void);
esp_err_t bsp_sd_unmount(void);

/* ---------------- Indicators ---------------- */

/* RGB LED is active-low on this board; this wrapper takes normal logic. */
void bsp_led_set(bool r, bool g, bool b);

#ifdef __cplusplus
}
#endif
