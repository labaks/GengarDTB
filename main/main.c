/*
 * deskos — desk assistant shell for the ESP32-2432S028R (CYD).
 * Board facts, architecture decisions and known constraints: see ../CLAUDE.md
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "bsp.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host.h"
#include "input.h"
#include "net.h"
#include "nvs_flash.h"
#include "shell.h"
#include "app_registry.h"

static const char *TAG = "main";

/* ---------------------------------------------------------- built-in apps */

/* Built-in widgets are embedded in the image and unpacked to LittleFS on boot.
 * They could have been drawn by compiled-in C, but then there would be two
 * different ways to run a widget and only one of them would get exercised.
 * Unpacking keeps a single loader — the filesystem one — and means the device
 * is useful before any microSD card exists. */
#define EMBEDDED(name) \
    extern const char name##_start[] asm("_binary_" #name "_start"); \
    extern const char name##_end[]   asm("_binary_" #name "_end")

/* EMBED_TXTFILES appends a NUL byte that the _end symbol counts, so the payload
 * is one shorter than the symbols suggest. The subtraction happens on the
 * difference, never on the pointer: `_end - 1` reads to the compiler as index
 * -1 of an extern array and trips -Werror=array-bounds. */
#define EMB_LEN(name) ((size_t)(name##_end - name##_start) - 1)

EMBEDDED(hello_manifest_json);
EMBEDDED(hello_ui_jsonl);
EMBEDDED(weather_manifest_json);
EMBEDDED(weather_ui_jsonl);
EMBEDDED(clock_manifest_json);
EMBEDDED(clock_ui_jsonl);
EMBEDDED(system_manifest_json);
EMBEDDED(system_ui_jsonl);

/* Compares CONTENT, not just size. Size alone is a trap: changing the weather
 * widget's coordinates from Moscow to Plovdiv altered no byte count at all, so
 * a size check silently kept serving the old file. */
static bool already_current(const char *path, const char *data, size_t len)
{
    struct stat st;
    if (stat(path, &st) != 0 || (size_t)st.st_size != len) {
        return false;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }

    char buf[256];
    size_t off = 0;
    bool same = true;

    while (off < len) {
        const size_t want = (len - off > sizeof(buf)) ? sizeof(buf) : (len - off);
        if (fread(buf, 1, want, f) != want || memcmp(buf, data + off, want) != 0) {
            same = false;
            break;
        }
        off += want;
    }

    fclose(f);
    return same;
}

static void write_if_changed(const char *path, const char *data, size_t len)
{
    if (already_current(path, data, len)) {
        return;   /* do not burn a flash erase cycle on every boot */
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "cannot write %s: %s", path, strerror(errno));
        return;
    }
    fwrite(data, 1, len, f);
    fclose(f);
    ESP_LOGI(TAG, "unpacked %s (%u bytes)", path, (unsigned)len);
}

static void provision_builtin(const char *id,
                              const char *manifest, size_t manifest_len,
                              const char *ui, size_t ui_len)
{
    char dir[80];
    snprintf(dir, sizeof(dir), "%s/apps/%s", BSP_FS_MOUNT_POINT, id);
    mkdir(dir, 0777);

    char path[128];
    snprintf(path, sizeof(path), "%s/manifest.json", dir);
    write_if_changed(path, manifest, manifest_len);

    snprintf(path, sizeof(path), "%s/ui.jsonl", dir);
    write_if_changed(path, ui, ui_len);
}

static void provision_builtin_apps(void)
{
    char apps[64];
    snprintf(apps, sizeof(apps), "%s/apps", BSP_FS_MOUNT_POINT);
    mkdir(apps, 0777);

    provision_builtin("hello",
                      hello_manifest_json_start, EMB_LEN(hello_manifest_json),
                      hello_ui_jsonl_start,      EMB_LEN(hello_ui_jsonl));

    provision_builtin("weather",
                      weather_manifest_json_start, EMB_LEN(weather_manifest_json),
                      weather_ui_jsonl_start,      EMB_LEN(weather_ui_jsonl));

    provision_builtin("clock",
                      clock_manifest_json_start, EMB_LEN(clock_manifest_json),
                      clock_ui_jsonl_start,      EMB_LEN(clock_ui_jsonl));

    provision_builtin("system",
                      system_manifest_json_start, EMB_LEN(system_manifest_json),
                      system_ui_jsonl_start,      EMB_LEN(system_ui_jsonl));
}

/* The buttons are not soldered yet. Until they are, boot with the touch-zone
 * backend (usable but single-touch) and run the chord self-test below through
 * the injection backend, since a resistive panel can never produce a chord. */
#define DESKOS_CHORD_SELFTEST 0

#if DESKOS_CHORD_SELFTEST
static void chord_selftest(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(2000));

    ESP_LOGI(TAG, "--- chord self-test: watch for 'shell: input: ...' lines ---");
    const input_backend_t saved = input_get_backend();
    input_set_backend(INPUT_BACKEND_DEBUG_INJECT);

    /* mask, hold_ms, gap_ms after release, expectation.
     *
     * gap matters: the two B2 taps must land closer together than INPUT_DOUBLE_MS
     * or no double-click is produced. hold + gap is the interval between the two
     * CLICK events, so it has to stay comfortably under 300 ms. */
    static const struct {
        uint8_t     mask;
        uint32_t    hold;
        uint32_t    gap;
        const char *expect;
    } script[] = {
        { INPUT_B1,             80, 250, "B1 click" },
        { INPUT_B2,             80,  80, "B2 click" },
        { INPUT_B2,             80, 250, "B2 click + B2 dblclick" },
        { INPUT_B1 | INPUT_B2, 120, 250, "B1+B2 click (backlight step)" },
        { INPUT_B1 | INPUT_B3, 120, 250, "B1+B3 click (home)" },
        { INPUT_B3,            700, 250, "B3 long + B3 release" },
        { INPUT_MASK_ALL,      700, 250, "B1+B2+B3 long (escape hatch)" },
    };

    for (size_t i = 0; i < sizeof(script) / sizeof(script[0]); i++) {
        ESP_LOGI(TAG, "inject -> expect: %s", script[i].expect);
        input_inject(script[i].mask);
        vTaskDelay(pdMS_TO_TICKS(script[i].hold));
        input_inject(0);
        vTaskDelay(pdMS_TO_TICKS(script[i].gap));
    }

    ESP_LOGI(TAG, "--- chord self-test done, back to %d ---", (int)saved);
    input_set_backend(saved);
    vTaskDelete(NULL);
}
#endif

/* Set to 1 to skip the shell and run the display bring-up diagnostic instead:
 * it bypasses LVGL entirely, reads the controller ID, and paints solid frames at
 * several SPI clocks so a human can say which stage looked clean. */
#define DESKOS_DISPLAY_DIAG 0

#if DESKOS_DISPLAY_DIAG
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"

#define DIAG_CHUNK_LINES 40

/* esp_lcd_panel_draw_bitmap ships the buffer to the panel byte for byte, and the
 * ILI9341 wants RGB565 high byte first. A uint16_t on this CPU is little-endian,
 * so every constant must be byte-swapped for the diagnostic to be a TRUSTWORTHY
 * colour reference. Without this the test would show the very fault it is meant
 * to detect. */
#define RGB(c) ((uint16_t)__builtin_bswap16((uint16_t)(c)))

#define C_RED    RGB(0xF800)
#define C_GREEN  RGB(0x07E0)
#define C_BLUE   RGB(0x001F)
#define C_WHITE  RGB(0xFFFF)
#define C_GRAY   RGB(0x8410)
#define C_BLACK  RGB(0x0000)

static esp_lcd_panel_handle_t s_diag_panel;

/* Splits the rectangle so no single SPI transfer exceeds the bus limit set in
 * bsp_display.c. Getting this wrong is silent: an oversized draw_bitmap is
 * rejected and the panel simply keeps showing whatever was there before, which
 * looks exactly like a rendering bug somewhere else entirely. */
static void diag_rect(int x, int y, int w, int h, uint16_t raw)
{
    const size_t max_bytes = (size_t)BSP_LCD_H_RES * 80 * sizeof(uint16_t);

    int lines = (int)(max_bytes / ((size_t)w * sizeof(uint16_t)));
    if (lines < 1) {
        lines = 1;
    }
    if (lines > h) {
        lines = h;
    }

    uint16_t *buf = heap_caps_malloc((size_t)w * lines * sizeof(uint16_t), MALLOC_CAP_DMA);
    if (!buf) {
        ESP_LOGE(TAG, "diag: out of DMA memory for %dx%d", w, lines);
        return;
    }
    for (size_t i = 0; i < (size_t)w * lines; i++) {
        buf[i] = raw;
    }

    for (int yy = y; yy < y + h; yy += lines) {
        const int n = (yy + lines > y + h) ? (y + h - yy) : lines;
        const esp_err_t err = esp_lcd_panel_draw_bitmap(s_diag_panel, x, yy, x + w, yy + n, buf);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "draw_bitmap(%d,%d,%dx%d) failed: %s",
                     x, yy, w, n, esp_err_to_name(err));
            break;
        }
    }
    free(buf);
}

static void diag_fill(int w, int h, uint16_t raw)
{
    diag_rect(0, 0, w, h, raw);
}

/* SPI is deliberately dropped to 20 MHz for the whole diagnostic so that clock
 * rate cannot be confused with a geometry or driver fault. */
#define DIAG_PCLK (20 * 1000 * 1000)

/* Colour-order probe.
 *
 * Geometry and driver are settled (ST7789, swap_xy + mirror_x). What is left is
 * two independent binary unknowns: the byte order of each RGB565 word on the
 * wire, and whether the panel is wired RGB or BGR. Four combinations, so rather
 * than reason about them we show all four and let a human read the answer off
 * the screen. Swapping the R and B fields in software is equivalent to flipping
 * rgb_ele_order, which keeps this to a single panel init.
 *
 * Loops forever on purpose: a one-shot sweep is easy to miss. */
static uint16_t mk_colour(uint8_t r5, uint8_t g6, uint8_t b5, bool rb_swap, bool byte_swap)
{
    const uint16_t v = rb_swap ? (uint16_t)((b5 << 11) | (g6 << 5) | r5)
                               : (uint16_t)((r5 << 11) | (g6 << 5) | b5);
    return byte_swap ? (uint16_t)__builtin_bswap16(v) : v;
}

static void colour_probe(void)
{
    esp_lcd_panel_io_handle_t io = NULL;
    ESP_ERROR_CHECK(bsp_display_init_driver(BSP_LCD_DRIVER_ST7789, DIAG_PCLK, true,
                                            &io, &s_diag_panel));
    bsp_backlight_set(100);

    const int w = BSP_LCD_H_RES, h = BSP_LCD_V_RES, band = h / 3;

    ESP_LOGW(TAG, "=== COLOUR PROBE, loops forever ===");
    ESP_LOGW(TAG, "  each combo paints three bands: TOP, MIDDLE, BOTTOM");
    ESP_LOGW(TAG, "  the CORRECT combo shows TOP=RED, MIDDLE=GREEN, BOTTOM=BLUE");
    ESP_LOGW(TAG, "  tell me the combo number that looks right");

    const struct { bool rb, bs; } combos[] = {
        { false, true  },   /* 1: byte-swapped, RGB   (what we ship today) */
        { true,  true  },   /* 2: byte-swapped, R/B exchanged              */
        { false, false },   /* 3: raw order,    RGB                        */
        { true,  false },   /* 4: raw order,    R/B exchanged              */
    };

    for (;;) {
        for (unsigned i = 0; i < 4; i++) {
            ESP_LOGW(TAG, "  >>> COMBO %u: rb_swap=%d byte_swap=%d",
                     i + 1, combos[i].rb, combos[i].bs);

            diag_rect(0, 0,          w, band,          mk_colour(31, 0,  0,  combos[i].rb, combos[i].bs));
            diag_rect(0, band,       w, band,          mk_colour(0,  63, 0,  combos[i].rb, combos[i].bs));
            diag_rect(0, 2 * band,   w, h - 2 * band,  mk_colour(0,  0,  31, combos[i].rb, combos[i].bs));

            /* The combo number, on screen. Without this the viewer sees colours
             * but has no way to tell which combination produced them — the log
             * lives on the other end of a cable they are not watching.
             * Black reads unambiguously on every colour we paint here, and it is
             * the one value that is identical in all four encodings. */
            for (unsigned k = 0; k <= i; k++) {
                diag_rect(12 + (int)k * 26, 12, 18, 18, 0x0000);
            }

            vTaskDelay(pdMS_TO_TICKS(5000));
        }
    }
}

static void diag_stage(unsigned n, bsp_lcd_driver_t drv, bool landscape)
{
    esp_lcd_panel_io_handle_t io = NULL;
    s_diag_panel = NULL;

    if (bsp_display_init_driver(drv, DIAG_PCLK, landscape, &io, &s_diag_panel) != ESP_OK) {
        ESP_LOGE(TAG, "STAGE %u: init failed", n);
        return;
    }
    bsp_backlight_set(100);

    const int w = landscape ? BSP_LCD_H_RES : BSP_LCD_V_RES;
    const int h = landscape ? BSP_LCD_V_RES : BSP_LCD_H_RES;

    ESP_LOGW(TAG, ">>> STAGE %u: %s, %s (%dx%d)", n,
             drv == BSP_LCD_DRIVER_ST7789 ? "ST7789" : "ILI9341",
             landscape ? "landscape" : "native", w, h);

    /* Solid red first: the only question here is whether the WHOLE screen fills. */
    diag_fill(w, h, C_RED);
    vTaskDelay(pdMS_TO_TICKS(1800));

    /* Then the geometry pattern. */
    diag_fill(w, h, C_WHITE);
    diag_rect(0, 0, w, 10, C_GREEN);              /* stripe along the top edge  */
    diag_rect(0, 0, 56, 56, C_RED);               /* marker: TOP-LEFT           */
    diag_rect(w - 56, h - 56, 56, 56, C_BLUE);    /* marker: BOTTOM-RIGHT       */
    vTaskDelay(pdMS_TO_TICKS(4500));

    bsp_display_deinit(io, s_diag_panel);
}

/* Geometry probe, built from black and white only.
 *
 * Colour reports have been contradictory, and colour cannot be trusted to
 * describe geometry while the encoding itself is in question. Black (0x0000)
 * and white (0xFFFF) are the two values that are identical under every byte
 * order and channel order, so this draws with nothing else.
 *
 * Rendered one row at a time (320 px = 640 bytes, far below the transfer limit),
 * which makes the row stride explicit. A closed border plus a straight diagonal
 * means width, height and stride all agree. A diagonal that bends, or a border
 * that fails to close, means the panel is consuming a different number of bytes
 * per pixel than we are sending. */
static void geometry_draw(int w, int h, unsigned combo)
{
    uint16_t *row = heap_caps_malloc((size_t)w * sizeof(uint16_t), MALLOC_CAP_DMA);
    if (!row) {
        ESP_LOGE(TAG, "geometry probe: out of DMA memory");
        return;
    }

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            row[x] = C_BLACK;
        }

        if (y == 0 || y == h - 1) {
            for (int x = 0; x < w; x++) {
                row[x] = C_WHITE;                  /* top and bottom border     */
            }
        } else {
            row[0] = row[w - 1] = C_WHITE;         /* left and right border     */
            row[(y * w) / h] = C_WHITE;            /* diagonal, TL -> BR        */
        }

        /* Combo counter: `combo` white ticks in the TOP-LEFT area, and a solid
         * red block marking the TOP-LEFT corner so mirroring is unambiguous. */
        if (y >= 8 && y < 26) {
            for (unsigned k = 0; k < combo; k++) {
                for (int x = 8 + (int)k * 24; x < 8 + (int)k * 24 + 16 && x < w; x++) {
                    row[x] = C_WHITE;
                }
            }
        }
        if (y >= 34 && y < 74) {
            for (int x = 8; x < 48 && x < w; x++) {
                row[x] = C_RED;
            }
        }

        esp_lcd_panel_draw_bitmap(s_diag_panel, 0, y, w, y + 1, row);
    }
    free(row);
}

static void geometry_probe(void)
{
    ESP_LOGW(TAG, "=== GEOMETRY PROBE, loops forever ===");
    ESP_LOGW(TAG, "  looking for: a WHITE BORDER that touches ALL FOUR edges,");
    ESP_LOGW(TAG, "               a STRAIGHT diagonal running TOP-LEFT to BOTTOM-RIGHT,");
    ESP_LOGW(TAG, "               the RED block and the tick marks in the TOP-LEFT corner.");
    ESP_LOGW(TAG, "  ticks = combo number. Report the combo that satisfies all three.");

    const struct { bool swap, mx, my; const char *what; } combos[] = {
        { false, false, false, "native 240x320" },
        { true,  true,  false, "landscape, mirror_x" },
        { true,  false, true,  "landscape, mirror_y" },
        { true,  false, false, "landscape, no mirror" },
        { true,  true,  true,  "landscape, both mirrors" },
    };
    const unsigned n = sizeof(combos) / sizeof(combos[0]);

    esp_lcd_panel_io_handle_t io = NULL;
    ESP_ERROR_CHECK(bsp_display_init_driver(BSP_LCD_DRIVER_ST7789, DIAG_PCLK, false,
                                            &io, &s_diag_panel));
    bsp_backlight_set(100);

    for (;;) {
        for (unsigned i = 0; i < n; i++) {
            ESP_LOGW(TAG, "  >>> COMBO %u (%u ticks): %s",
                     i + 1, i + 1, combos[i].what);
            bsp_display_set_rotation(s_diag_panel, combos[i].swap, combos[i].mx, combos[i].my);

            const int w = combos[i].swap ? BSP_LCD_H_RES : BSP_LCD_V_RES;
            const int h = combos[i].swap ? BSP_LCD_V_RES : BSP_LCD_H_RES;
            geometry_draw(w, h, i + 1);

            vTaskDelay(pdMS_TO_TICKS(6000));
        }
    }
}

static void display_diag(void)
{
    geometry_probe();
    return;

    /* Driver and geometry are settled; go straight to the colour question. */
    colour_probe();   /* never returns */

    ESP_LOGW(TAG, "=== display diagnostic: 4 stages, SPI fixed at 20 MHz ===");
    ESP_LOGW(TAG, "  each stage: (1) full RED  -> does the ENTIRE screen fill?");
    ESP_LOGW(TAG, "              (2) WHITE with GREEN stripe on TOP edge,");
    ESP_LOGW(TAG, "                  RED square TOP-LEFT, BLUE square BOTTOM-RIGHT");
    ESP_LOGW(TAG, "  report for each stage: full coverage yes/no, pattern correct yes/no");

    diag_stage(1, BSP_LCD_DRIVER_ILI9341, false);   /* native  240x320 */
    diag_stage(2, BSP_LCD_DRIVER_ILI9341, true);    /* rotated 320x240 */
    diag_stage(3, BSP_LCD_DRIVER_ST7789,  false);   /* native  240x320 */
    diag_stage(4, BSP_LCD_DRIVER_ST7789,  true);    /* rotated 320x240 */

    ESP_LOGW(TAG, "=== diagnostic done — screen left on STAGE 4 ===");
}
#endif /* DESKOS_DISPLAY_DIAG */

void app_main(void)
{
    ESP_LOGI(TAG, "deskos booting, free heap %lu", (unsigned long)esp_get_free_heap_size());

#if DESKOS_DISPLAY_DIAG
    display_diag();
    return;
#endif

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(bsp_fs_mount());

    /* EMBED_TXTFILES appends a NUL that is counted in the _end symbol; the -1
     * above keeps it out of the files we write. */
    provision_builtin_apps();

    /* No card is a normal state, not a failure: the shell boots and says so. */
    if (bsp_sd_mount() != ESP_OK) {
        ESP_LOGW(TAG, "continuing without microSD — no widget apps will be listed");
    }

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_handle_t panel = NULL;
    /* Landscape is set on the panel here; LVGL must not rotate again. */
    ESP_ERROR_CHECK(bsp_display_init(0, true, &io, &panel));
    ESP_ERROR_CHECK(bsp_touch_init());

    /* Only B1 (GPIO22) and B2 (GPIO27) are wired, both on the CN1 connector
     * where the internal pull-ups suffice. B3 lives on GPIO35, which has no
     * internal pull-up and would float without its external 10k — declaring it
     * absent keeps the pin unread rather than inventing keypresses. */
    input_set_present_mask(INPUT_B1 | INPUT_B2);
    ESP_ERROR_CHECK(input_init(INPUT_BACKEND_GPIO));

    app_registry_scan();

    /* TEMP diagnostic for the api.open-meteo.com TLS failure (open question in
     * CLAUDE.md) — prints which bundled root esp_crt_bundle matched and why
     * mbedtls_pk_verify_ext rejected it. Needs CONFIG_LOG_MAXIMUM_LEVEL_DEBUG
     * in sdkconfig to actually compile the message in. Remove once diagnosed. */
    esp_log_level_set("esp-x509-crt-bundle", ESP_LOG_DEBUG);

    /* Brought up before the shell so the launcher's status line is right from
     * the first frame. Returns OK with no credentials — that is a state to
     * display, not a boot failure. */
    ESP_ERROR_CHECK(net_init());

    /* Same reasoning as net_init() above: up before the shell so the "pc"
     * status reflects reality from the first frame. No agent configured (no
     * /sd/agent.json) is a state to display, not a boot failure. */
    ESP_ERROR_CHECK(host_init());

    ESP_ERROR_CHECK(shell_start(io, panel));

    /* Confirms to the bootloader that this boot is good, cancelling the
     * rollback-on-crash-loop protection (ROADMAP #17). Must happen after a
     * boot has actually gone well, and before anything ever calls
     * ota_check_and_update() again — starting a new OTA download while the
     * running image is still in the unconfirmed PENDING_VERIFY state is a
     * hard error (esp_ota_ops.h). Harmless — just logged, not asserted — on
     * a boot that was never OTA'd into in the first place (esptool flashing
     * over USB never puts a partition into that state), which is every boot
     * so far. */
    const esp_err_t rollback_err = esp_ota_mark_app_valid_cancel_rollback();
    if (rollback_err != ESP_OK) {
        ESP_LOGI(TAG, "rollback confirm: %s (normal outside an actual OTA update)",
                 esp_err_to_name(rollback_err));
    }

    ESP_LOGI(TAG, "boot complete, free heap %lu", (unsigned long)esp_get_free_heap_size());

#if DESKOS_CHORD_SELFTEST
    xTaskCreatePinnedToCore(chord_selftest, "chordtest", 3072, NULL, 3, NULL, 0);
#endif
}
