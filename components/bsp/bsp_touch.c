/*
 * XPT2046 resistive touch over BIT-BANGED SPI.
 *
 * Why not hardware SPI or the off-the-shelf esp_lcd_touch_xpt2046 component:
 * the CYD wires touch and the SD card to different pin sets on the same nominal
 * bus, and the ESP32 has only two usable SPI controllers for three peripherals.
 * SD gets the real bus (it needs the bandwidth); touch gets bit-banging, which is
 * perfectly adequate for a converter that tops out around 2 MHz.
 *
 * NOTE: this panel is RESISTIVE, therefore SINGLE-TOUCH ONLY. Button chords cannot
 * be tested through the touchscreen — use the debug_inject input backend for that.
 */
#include "bsp.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "bsp.touch";

#define NVS_NAMESPACE  "deskos"
#define NVS_KEY_TOUCH  "touch_cal"

/* XPT2046 control bytes: differential mode, 12-bit, power-down between conversions. */
#define CMD_READ_X  0xD0
#define CMD_READ_Y  0x90
#define CMD_READ_Z1 0xB0

/* Below this pressure reading we treat the panel as untouched. Raised from the
 * textbook value because the CYD's flex cable picks up noise from the backlight PWM. */
#define Z_THRESHOLD 400

/* Typical raw span for an XPT2046, in ascending order — NOT a claim about
 * which end is which; that's what calibration determines. Every panel differs
 * by a few percent, so these are a starting point, not a calibration. */
static uint16_t s_x_lo = 300, s_x_hi = 3800;
static uint16_t s_y_lo = 300, s_y_hi = 3800;

static inline void clk(bool level)
{
    gpio_set_level(BSP_TOUCH_PIN_CLK, level);
    esp_rom_delay_us(1);            /* ~500 kHz; well inside the chip's limits */
}

static uint16_t xpt_transfer(uint8_t cmd)
{
    gpio_set_level(BSP_TOUCH_PIN_CS, 0);

    /* Command: 8 bits, MSB first, shifted out while the clock rises. */
    for (int i = 7; i >= 0; i--) {
        gpio_set_level(BSP_TOUCH_PIN_MOSI, (cmd >> i) & 1);
        clk(1);
        clk(0);
    }

    /* One clock for the conversion/BUSY period, then 12 data bits MSB first.
     * The XPT2046 shifts data out on the falling edge, so we sample while high. */
    clk(1);
    clk(0);

    uint16_t value = 0;
    for (int i = 0; i < 12; i++) {
        clk(1);
        value = (value << 1) | (uint16_t)gpio_get_level(BSP_TOUCH_PIN_MISO);
        clk(0);
    }

    gpio_set_level(BSP_TOUCH_PIN_CS, 1);
    return value;
}

/* Median of three: the cheapest effective filter for this converter's jitter. */
static uint16_t read_median3(uint8_t cmd)
{
    uint16_t a = xpt_transfer(cmd);
    uint16_t b = xpt_transfer(cmd);
    uint16_t c = xpt_transfer(cmd);

    if (a > b) { uint16_t t = a; a = b; b = t; }
    if (b > c) { uint16_t t = b; b = c; c = t; }
    if (a > b) { uint16_t t = a; a = b; b = t; }
    return b;
}

esp_err_t bsp_touch_init(void)
{
    const gpio_config_t out = {
        .pin_bit_mask = (1ULL << BSP_TOUCH_PIN_CLK) |
                        (1ULL << BSP_TOUCH_PIN_MOSI) |
                        (1ULL << BSP_TOUCH_PIN_CS),
        .mode         = GPIO_MODE_OUTPUT,
    };
    esp_err_t err = gpio_config(&out);
    if (err != ESP_OK) {
        return err;
    }

    /* GPIO36/39 are input-only pins with no pull resistors available — that is fine,
     * MISO is driven by the converter and PENIRQ has an external pull-up on the board. */
    const gpio_config_t in = {
        .pin_bit_mask = (1ULL << BSP_TOUCH_PIN_MISO) | (1ULL << BSP_TOUCH_PIN_IRQ),
        .mode         = GPIO_MODE_INPUT,
    };
    err = gpio_config(&in);
    if (err != ESP_OK) {
        return err;
    }

    gpio_set_level(BSP_TOUCH_PIN_CS, 1);
    gpio_set_level(BSP_TOUCH_PIN_CLK, 0);

    ESP_LOGI(TAG, "XPT2046 (bit-bang) ready, single-touch");
    return ESP_OK;
}

void bsp_touch_set_calibration(uint16_t x_lo, uint16_t x_hi,
                               uint16_t y_lo, uint16_t y_hi)
{
    /* lo/hi are "raw reading at output 0 / output_max", not a numeric
     * min/max — a reversed axis (lo > hi) is valid, see map_clamped. Equal
     * is the only real error: a calibration attempt that measured nothing. */
    if (x_hi != x_lo && y_hi != y_lo) {
        s_x_lo = x_lo;
        s_x_hi = x_hi;
        s_y_lo = y_lo;
        s_y_hi = y_hi;
    }
}

typedef struct {
    uint16_t x_lo, x_hi, y_lo, y_hi;
} touch_cal_blob_t;

esp_err_t bsp_touch_load_calibration(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }

    touch_cal_blob_t blob;
    size_t len = sizeof(blob);
    err = nvs_get_blob(h, NVS_KEY_TOUCH, &blob, &len);
    nvs_close(h);

    if (err != ESP_OK || len != sizeof(blob)) {
        return (err == ESP_OK) ? ESP_ERR_NVS_NOT_FOUND : err;
    }

    bsp_touch_set_calibration(blob.x_lo, blob.x_hi, blob.y_lo, blob.y_hi);
    ESP_LOGI(TAG, "calibration loaded: x[%u,%u] y[%u,%u]",
             blob.x_lo, blob.x_hi, blob.y_lo, blob.y_hi);
    return ESP_OK;
}

esp_err_t bsp_touch_save_calibration(uint16_t x_lo, uint16_t x_hi,
                                     uint16_t y_lo, uint16_t y_hi)
{
    /* Apply first: a device that fails to persist should still have the
     * calibration it was just given for the rest of this session. */
    bsp_touch_set_calibration(x_lo, x_hi, y_lo, y_hi);

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    const touch_cal_blob_t blob = { x_lo, x_hi, y_lo, y_hi };
    err = nvs_set_blob(h, NVS_KEY_TOUCH, &blob, sizeof(blob));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

bool bsp_touch_read_raw(uint16_t *out_x, uint16_t *out_y, uint16_t *out_z)
{
    /* PENIRQ is active low and costs nothing to check, so bail out early when
     * nobody is touching the panel rather than clocking out 9 conversions.
     *
     * This reports physical contact (the PENIRQ signal itself), not "pressed
     * hard enough to count as a click" — Z_THRESHOLD is a separate debounce
     * for that, applied by bsp_touch_read(). A light, precisely-aimed touch
     * (e.g. a calibration target) can sit well under that threshold while
     * still being a perfectly real touch; conflating the two here made the
     * calibration screen unusable — see CLAUDE.md. */
    if (gpio_get_level(BSP_TOUCH_PIN_IRQ) != 0) {
        if (out_z) {
            *out_z = 0;
        }
        return false;
    }

    const uint16_t z = read_median3(CMD_READ_Z1);
    const uint16_t x = read_median3(CMD_READ_X);
    const uint16_t y = read_median3(CMD_READ_Y);

    if (out_x) { *out_x = x; }
    if (out_y) { *out_y = y; }
    if (out_z) { *out_z = z; }

    return true;
}

/* lo is "the raw reading at output 0", hi is "the raw reading at output_max" —
 * NOT a numeric min/max. Resistive panels can be wired either way round on a
 * given axis (confirmed on this board: Y increases with the reading, X does
 * not), so lo > hi is a legitimate, calibrated reversed axis, not an error. */
static int16_t map_clamped(uint16_t raw, uint16_t lo, uint16_t hi, int16_t out_max)
{
    if (lo == hi) {
        return 0;   /* degenerate calibration; avoid a divide by zero */
    }
    if (lo < hi) {
        if (raw <= lo) { return 0; }
        if (raw >= hi) { return out_max; }
        return (int16_t)(((uint32_t)(raw - lo) * out_max) / (hi - lo));
    }
    if (raw >= lo) { return 0; }
    if (raw <= hi) { return out_max; }
    return (int16_t)(((uint32_t)(lo - raw) * out_max) / (lo - hi));
}

void bsp_touch_read(bsp_touch_state_t *out)
{
    if (!out) {
        return;
    }

    uint16_t raw_x = 0, raw_y = 0, raw_z = 0;
    if (!bsp_touch_read_raw(&raw_x, &raw_y, &raw_z) || raw_z <= Z_THRESHOLD) {
        out->pressed = false;
        return;
    }

    /* The panel's native orientation is portrait 240x320 while we run landscape
     * 320x240, so raw X drives screen Y and vice versa. The Y axis also runs
     * backwards relative to the display, hence the inversion. */
    out->x = map_clamped(raw_y, s_y_lo, s_y_hi, BSP_LCD_H_RES - 1);
    out->y = (BSP_LCD_V_RES - 1) - map_clamped(raw_x, s_x_lo, s_x_hi, BSP_LCD_V_RES - 1);
    out->pressed = true;
}
