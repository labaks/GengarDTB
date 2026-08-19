#include "bsp.h"

#include <string.h>

#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_ili9341.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "nvs.h"

static const char *TAG = "bsp.disp";

#define NVS_NAMESPACE      "deskos"
#define NVS_KEY_BACKLIGHT  "backlight"

#define BL_LEDC_TIMER    LEDC_TIMER_0
#define BL_LEDC_CHANNEL  LEDC_CHANNEL_0
#define BL_LEDC_MODE     LEDC_LOW_SPEED_MODE
#define BL_DUTY_RES      LEDC_TIMER_10_BIT
#define BL_DUTY_MAX      1023
#define BL_FREQ_HZ       5000

static uint8_t s_backlight_pct;

static esp_err_t backlight_init(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode      = BL_LEDC_MODE,
        .timer_num       = BL_LEDC_TIMER,
        .duty_resolution = BL_DUTY_RES,
        .freq_hz         = BL_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "ledc timer");

    const ledc_channel_config_t ch = {
        .gpio_num   = BSP_LCD_PIN_BL,
        .speed_mode = BL_LEDC_MODE,
        .channel    = BL_LEDC_CHANNEL,
        .timer_sel  = BL_LEDC_TIMER,
        .duty       = 0,
        .hpoint     = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&ch), TAG, "ledc channel");
    return ESP_OK;
}

esp_err_t bsp_backlight_set(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }
    s_backlight_pct = percent;

    const uint32_t duty = ((uint32_t)percent * BL_DUTY_MAX) / 100u;
    ESP_RETURN_ON_ERROR(ledc_set_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL, duty), TAG, "set duty");
    return ledc_update_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL);
}

uint8_t bsp_backlight_get(void)
{
    return s_backlight_pct;
}

esp_err_t bsp_backlight_save(uint8_t percent)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h), TAG, "nvs open");
    esp_err_t err = nvs_set_u8(h, NVS_KEY_BACKLIGHT, percent);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

/* Query only — does not call bsp_backlight_set. The caller decides when it is
 * actually safe to light the panel (see shell_start: only after the first
 * frame is drawn), so applying the loaded value is left to them. */
uint8_t bsp_backlight_load(uint8_t fallback_pct)
{
    nvs_handle_t h;
    uint8_t pct = fallback_pct;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, NVS_KEY_BACKLIGHT, &pct);
        nvs_close(h);
    }
    return pct;
}

esp_err_t bsp_display_deinit(esp_lcd_panel_io_handle_t io, esp_lcd_panel_handle_t panel)
{
    if (panel) {
        esp_lcd_panel_del(panel);
    }
    if (io) {
        esp_lcd_panel_io_del(io);
    }
    return ESP_OK;
}

esp_err_t bsp_display_set_rotation(esp_lcd_panel_handle_t panel,
                                   bool swap_xy, bool mirror_x, bool mirror_y)
{
    ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(panel, swap_xy), TAG, "swap_xy");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(panel, mirror_x, mirror_y), TAG, "mirror");
    return ESP_OK;
}

esp_err_t bsp_display_probe_id(esp_lcd_panel_io_handle_t io,
                               uint8_t out_d3[3], uint8_t out_04[3])
{
    esp_err_t err = ESP_OK;

    if (out_d3) {
        memset(out_d3, 0, 3);
        err = esp_lcd_panel_io_rx_param(io, 0xD3, out_d3, 3);
    }
    if (out_04) {
        memset(out_04, 0, 3);
        const esp_err_t e2 = esp_lcd_panel_io_rx_param(io, 0x04, out_04, 3);
        if (err == ESP_OK) {
            err = e2;
        }
    }
    return err;
}

esp_err_t bsp_display_init(uint32_t pclk_hz, bool landscape,
                           esp_lcd_panel_io_handle_t *out_io,
                           esp_lcd_panel_handle_t *out_panel)
{
    /* ST7789, confirmed empirically on this board: with the ILI9341 driver the
     * address window wrapped, leaving partial fills and duplicated markers.
     * Under ST7789 a full-screen fill covers the whole panel and the corner
     * markers land where they are drawn. */
    return bsp_display_init_driver(BSP_LCD_DRIVER_ST7789, pclk_hz, landscape,
                                   out_io, out_panel);
}

esp_err_t bsp_display_init_driver(bsp_lcd_driver_t driver, uint32_t pclk_hz, bool landscape,
                                  esp_lcd_panel_io_handle_t *out_io,
                                  esp_lcd_panel_handle_t *out_panel)
{
    ESP_RETURN_ON_FALSE(out_io && out_panel, ESP_ERR_INVALID_ARG, TAG, "null out params");

    if (pclk_hz == 0) {
        pclk_hz = BSP_LCD_PIXEL_CLOCK_HZ;
    }

    ESP_RETURN_ON_ERROR(backlight_init(), TAG, "backlight");

    /* Keep the panel dark until LVGL has drawn its first frame, otherwise the user
     * sees a screenful of uninitialised RAM on every boot. */
    ESP_RETURN_ON_ERROR(bsp_backlight_set(0), TAG, "backlight off");

    const spi_bus_config_t bus = {
        .sclk_io_num     = BSP_LCD_PIN_SCLK,
        .mosi_io_num     = BSP_LCD_PIN_MOSI,
        .miso_io_num     = BSP_LCD_PIN_MISO,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        /* Must be >= the largest single draw_bitmap, not just the largest LVGL
         * flush. Sized for 80 lines rather than the 40 LVGL uses, because a
         * transfer over this limit is rejected — and esp_lcd_panel_draw_bitmap
         * reports that only through its return value, so an unchecked caller
         * sees the screen silently keep its old contents. */
        .max_transfer_sz = BSP_LCD_H_RES * 80 * sizeof(uint16_t),
    };
    const esp_err_t bus_err = spi_bus_initialize(BSP_LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    /* Tolerate a second call: the diagnostic re-inits the panel at other clocks
     * while the bus stays up. */
    if (bus_err != ESP_OK && bus_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "spi bus init: %s", esp_err_to_name(bus_err));
        return bus_err;
    }

    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num       = BSP_LCD_PIN_DC,
        .cs_gpio_num       = BSP_LCD_PIN_CS,
        .pclk_hz           = pclk_hz,
        .lcd_cmd_bits      = 8,
        .lcd_param_bits    = 8,
        .spi_mode          = 0,
        .trans_queue_depth = 10,
        /* on_color_trans_done is deliberately left unset: esp_lvgl_port installs
         * its own callback when we hand it this handle. */
    };
    esp_lcd_panel_io_handle_t io = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BSP_LCD_SPI_HOST,
                                                 &io_cfg, &io),
                        TAG, "panel io");

    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = BSP_LCD_PIN_RST,
        /* RGB, not BGR. Determined on the bench: with BGR the red and blue
         * channels came out exchanged. Much CYD material claims BGR — that
         * evidently does not hold for this batch. */
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    esp_lcd_panel_handle_t panel = NULL;
    if (driver == BSP_LCD_DRIVER_ST7789) {
        ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(io, &panel_cfg, &panel), TAG, "st7789");
    } else {
        ESP_RETURN_ON_ERROR(esp_lcd_new_panel_ili9341(io, &panel_cfg, &panel), TAG, "ili9341");
    }

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel), TAG, "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), TAG, "init");

    /* Some CYD batches ship an inverted panel. If the first boot shows a photo
     * negative, flip this to true — that is the whole fix. */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(panel, false), TAG, "invert");

    /* Rotation is applied to the PANEL here, not by esp_lvgl_port. The panel's
     * native orientation is 240x320 portrait; swapping axes gives the 320x240
     * landscape that LVGL is told about. Only one layer may rotate — if LVGL
     * also rotates, it flushes 320-wide rows into a 240-wide address window,
     * the writes wrap, and the screen fills with diagonal stripes over whatever
     * the previous firmware left in GRAM. */
    if (landscape) {
        ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(panel, true), TAG, "swap_xy");
        ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(panel, true, false), TAG, "mirror");
    }

    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(panel, true), TAG, "disp on");

    ESP_LOGI(TAG, "panel up: %s, %s, SPI2 @ %lu MHz",
             driver == BSP_LCD_DRIVER_ST7789 ? "ST7789" : "ILI9341",
             landscape ? "320x240 landscape" : "240x320 native",
             (unsigned long)(pclk_hz / 1000000));

    *out_io = io;
    *out_panel = panel;
    return ESP_OK;
}
