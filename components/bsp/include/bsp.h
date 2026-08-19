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

/* Which controller to drive. CYD batches are not consistent: some carry an
 * ILI9341, some an ST7789, and their command sets overlap enough that the wrong
 * driver "almost works" — partial fills, wrapped writes, striping. */
typedef enum {
    BSP_LCD_DRIVER_ILI9341,
    BSP_LCD_DRIVER_ST7789,
} bsp_lcd_driver_t;

/* Same as bsp_display_init but lets the caller pick the controller. */
esp_err_t bsp_display_init_driver(bsp_lcd_driver_t driver, uint32_t pclk_hz, bool landscape,
                                  esp_lcd_panel_io_handle_t *out_io,
                                  esp_lcd_panel_handle_t *out_panel);

/* Brings up SPI2, the ILI9341 panel and the LEDC backlight channel.
 * Hands back the esp_lcd handles so the caller can attach LVGL to them.
 *
 * pclk_hz: pass 0 for BSP_LCD_PIXEL_CLOCK_HZ. Overridable so the display
 * diagnostic can sweep clock rates without rebuilding.
 *
 * landscape: when true the panel itself is rotated to 320x240 here, via
 * swap_xy/mirror. Do NOT also ask esp_lvgl_port to rotate — doing both leaves
 * LVGL flushing 320-wide rows into a 240-wide address window, which wraps and
 * produces diagonal stripes over whatever was in GRAM before. */
esp_err_t bsp_display_init(uint32_t pclk_hz, bool landscape,
                           esp_lcd_panel_io_handle_t *out_io,
                           esp_lcd_panel_handle_t *out_panel);

/* Tears the panel down but leaves the SPI bus and backlight up, so the caller
 * can re-init at a different clock. Diagnostic use. */
esp_err_t bsp_display_deinit(esp_lcd_panel_io_handle_t io, esp_lcd_panel_handle_t panel);

/* Applies orientation to a live panel. Used by the diagnostic to sweep mirror
 * combinations without re-initialising anything. */
esp_err_t bsp_display_set_rotation(esp_lcd_panel_handle_t panel,
                                   bool swap_xy, bool mirror_x, bool mirror_y);

/* Reads the controller ID registers over MISO. ILI9341 answers 0xD3 with
 * 00 93 41; ST7789 answers 0x04 with 85 85 52. Returns ESP_OK if the bus
 * replied at all — the caller interprets the bytes. */
esp_err_t bsp_display_probe_id(esp_lcd_panel_io_handle_t io,
                               uint8_t out_d3[3], uint8_t out_04[3]);

/* 0..100. Perceptually non-linear (raw LEDC duty); good enough for a menu slider. */
esp_err_t bsp_backlight_set(uint8_t percent);
uint8_t   bsp_backlight_get(void);

/* NVS-backed persistence, same pattern as touch calibration. Load is a pure
 * query (returns fallback_pct if nothing saved) — it does not call
 * bsp_backlight_set, since the caller knows when it is actually safe to
 * light the panel. */
esp_err_t bsp_backlight_save(uint8_t percent);
uint8_t   bsp_backlight_load(uint8_t fallback_pct);

/* ---------------- Touch (XPT2046, software SPI) ---------------- */

typedef struct {
    int16_t x;          /* screen coordinates, already rotated to landscape */
    int16_t y;
    bool    pressed;
} bsp_touch_state_t;

esp_err_t bsp_touch_init(void);

/* Non-blocking. Returns the debounced current state; safe to poll from the LVGL task. */
void bsp_touch_read(bsp_touch_state_t *out);

/* Raw ADC readings, for building a calibration screen. Returns true on any
 * physical contact (PENIRQ), independent of pressure — bsp_touch_read()'s
 * Z_THRESHOLD click-debounce is not applied here, so a light, precisely
 * aimed touch (e.g. a calibration target) still counts as a touch. */
bool bsp_touch_read_raw(uint16_t *out_x, uint16_t *out_y, uint16_t *out_z);

/* lo/hi per axis: the raw reading at screen coordinate 0 and at screen
 * coordinate max, respectively — NOT a numeric min/max. A resistive panel can
 * be wired either way round on a given axis, so lo > hi is a legitimate,
 * calibrated reversed axis. Defaults are typical XPT2046 values and WILL be
 * off on any given panel — persist real ones once the calibration app exists. */
void bsp_touch_set_calibration(uint16_t x_lo, uint16_t x_hi,
                               uint16_t y_lo, uint16_t y_hi);

/* NVS-backed persistence for the above. Load returns ESP_ERR_NVS_NOT_FOUND on a
 * first boot with no saved calibration — that is the shell's cue to run the
 * calibration screen once, not a fault. Save both writes NVS and applies the
 * values immediately via bsp_touch_set_calibration. */
esp_err_t bsp_touch_load_calibration(void);
esp_err_t bsp_touch_save_calibration(uint16_t x_lo, uint16_t x_hi,
                                     uint16_t y_lo, uint16_t y_hi);

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
