/*
 * Board pin map — ESP32-2432S028R ("Cheap Yellow Display", CYD).
 *
 * Verified against two independent pinout references, not guessed from a datasheet.
 * If you port deskos to another board, this file plus bsp_touch.c are what change.
 *
 * THE BUS CONFLICT (read before touching anything here):
 *   Touch (25/32/39/33) and SD (18/23/19/5) are both nominally "VSPI", but they are wired
 *   to DIFFERENT pin sets. One SPI controller cannot serve two pin sets at once, and the
 *   ESP32 only has two usable SPI buses for three peripherals. SDMMC is no escape either —
 *   its slot pins (14/15/2) collide with the display.
 *   So: SPI2 -> display, SPI3 -> SD (it needs the bandwidth), touch -> software SPI.
 */
#pragma once

/* ---- Display: ILI9341, 320x240, on SPI2_HOST ---- */
#define BSP_LCD_SPI_HOST        SPI2_HOST
#define BSP_LCD_PIN_SCLK        14
#define BSP_LCD_PIN_MOSI        13
#define BSP_LCD_PIN_MISO        12
#define BSP_LCD_PIN_CS          15
#define BSP_LCD_PIN_DC          2
#define BSP_LCD_PIN_RST         (-1)    /* no reset line on this board */
#define BSP_LCD_PIN_BL          21      /* backlight, driven by LEDC */

#define BSP_LCD_H_RES           320
#define BSP_LCD_V_RES           240

/* 20 MHz is the rate this board was actually verified at, end to end, with a
 * closed border and a straight diagonal. 40 MHz is the usual figure quoted for
 * these panels and is probably fine, but it has not been proven here — raise it
 * only while watching for tearing or garbled rows. At 20 MHz a full-screen
 * repaint costs roughly 60 ms, and the shell repaints partial areas anyway. */
#define BSP_LCD_PIXEL_CLOCK_HZ  (20 * 1000 * 1000)

/* ---- Touch: XPT2046, bit-banged software SPI (see note above) ---- */
#define BSP_TOUCH_PIN_CLK       25
#define BSP_TOUCH_PIN_MOSI      32
#define BSP_TOUCH_PIN_MISO      39      /* input-only pin — fine, it is an input */
#define BSP_TOUCH_PIN_CS        33
#define BSP_TOUCH_PIN_IRQ       36      /* input-only, PENIRQ, active low */

/* ---- microSD: SPI3_HOST ---- */
#define BSP_SD_SPI_HOST         SPI3_HOST
#define BSP_SD_PIN_SCLK         18
#define BSP_SD_PIN_MOSI         23
#define BSP_SD_PIN_MISO         19
#define BSP_SD_PIN_CS           5
#define BSP_SD_MOUNT_POINT      "/sd"

/* ---- LittleFS on the internal "storage" partition ---- */
#define BSP_FS_PARTITION_LABEL  "storage"
#define BSP_FS_MOUNT_POINT      "/fs"

/* ---- Onboard extras ---- */
#define BSP_PIN_LED_R           4       /* RGB LED, ACTIVE LOW */
#define BSP_PIN_LED_G           16
#define BSP_PIN_LED_B           17
#define BSP_PIN_LDR             34      /* ambient light, ADC1 */
#define BSP_PIN_SPEAKER         26      /* DAC2 */

/* ---- Buttons: the only three free GPIOs on the whole board ---- */
#define BSP_PIN_BTN1            22      /* internal pull-up available */
#define BSP_PIN_BTN2            27      /* internal pull-up available */
#define BSP_PIN_BTN3            35      /* INPUT ONLY, NO internal pull-up:
                                         * needs an external 10k to 3V3 */
