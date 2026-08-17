#include "bsp.h"

#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_ili9341.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"

static const char *TAG = "bsp.disp";

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

esp_err_t bsp_display_init(esp_lcd_panel_io_handle_t *out_io,
                           esp_lcd_panel_handle_t *out_panel)
{
    ESP_RETURN_ON_FALSE(out_io && out_panel, ESP_ERR_INVALID_ARG, TAG, "null out params");

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
        /* Must be >= the largest LVGL flush. 40 lines of RGB565 is our buffer height. */
        .max_transfer_sz = BSP_LCD_H_RES * 40 * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(BSP_LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO),
                        TAG, "spi bus init");

    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num       = BSP_LCD_PIN_DC,
        .cs_gpio_num       = BSP_LCD_PIN_CS,
        .pclk_hz           = BSP_LCD_PIXEL_CLOCK_HZ,
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
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_BGR,  /* CYD panels are BGR-wired */
        .bits_per_pixel = 16,
    };
    esp_lcd_panel_handle_t panel = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_ili9341(io, &panel_cfg, &panel), TAG, "ili9341");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel), TAG, "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), TAG, "init");

    /* Some CYD batches ship an inverted panel. If the first boot shows a photo
     * negative, flip this to true — that is the whole fix. */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(panel, false), TAG, "invert");

    /* Landscape rotation is applied by esp_lvgl_port via its rotation config,
     * so we intentionally do NOT call swap_xy/mirror here — doing both
     * double-rotates and is a classic source of a mirrored UI. */

    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(panel, true), TAG, "disp on");

    ESP_LOGI(TAG, "ILI9341 up: %dx%d, SPI2 @ %d MHz",
             BSP_LCD_H_RES, BSP_LCD_V_RES, BSP_LCD_PIXEL_CLOCK_HZ / 1000000);

    *out_io = io;
    *out_panel = panel;
    return ESP_OK;
}
