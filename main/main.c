/*
 * deskos — desk assistant shell for the ESP32-2432S028R (CYD).
 * Board facts, architecture decisions and known constraints: see ../CLAUDE.md
 */
#include "bsp.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "input.h"
#include "nvs_flash.h"
#include "shell.h"
#include "app_registry.h"

static const char *TAG = "main";

/* The buttons are not soldered yet. Until they are, boot with the touch-zone
 * backend (usable but single-touch) and run the chord self-test below through
 * the injection backend, since a resistive panel can never produce a chord. */
#define DESKOS_CHORD_SELFTEST 1

#if DESKOS_CHORD_SELFTEST
static void chord_selftest(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(2000));

    ESP_LOGI(TAG, "--- chord self-test: watch for 'shell: input: ...' lines ---");
    const input_backend_t saved = input_get_backend();
    input_set_backend(INPUT_BACKEND_DEBUG_INJECT);

    /* mask, hold_ms — expectation is written next to each line. */
    static const struct { uint8_t mask; uint32_t hold; const char *expect; } script[] = {
        { INPUT_B1,                          80,  "B1 click" },
        { INPUT_B2,                          80,  "B2 click" },
        { INPUT_B2,                          80,  "B2 click + B2 dblclick" },
        { INPUT_B1 | INPUT_B2,              120,  "B1+B2 click (backlight step)" },
        { INPUT_B1 | INPUT_B3,              120,  "B1+B3 click (home)" },
        { INPUT_B3,                         700,  "B3 long + B3 release" },
        { INPUT_MASK_ALL,                   700,  "B1+B2+B3 long (escape hatch)" },
    };

    for (size_t i = 0; i < sizeof(script) / sizeof(script[0]); i++) {
        ESP_LOGI(TAG, "inject -> expect: %s", script[i].expect);
        input_inject(script[i].mask);
        vTaskDelay(pdMS_TO_TICKS(script[i].hold));
        input_inject(0);
        vTaskDelay(pdMS_TO_TICKS(250));
    }

    ESP_LOGI(TAG, "--- chord self-test done, back to %d ---", (int)saved);
    input_set_backend(saved);
    vTaskDelete(NULL);
}
#endif

void app_main(void)
{
    ESP_LOGI(TAG, "deskos booting, free heap %lu", (unsigned long)esp_get_free_heap_size());

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(bsp_fs_mount());

    /* No card is a normal state, not a failure: the shell boots and says so. */
    if (bsp_sd_mount() != ESP_OK) {
        ESP_LOGW(TAG, "continuing without microSD — no widget apps will be listed");
    }

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_handle_t panel = NULL;
    ESP_ERROR_CHECK(bsp_display_init(&io, &panel));
    ESP_ERROR_CHECK(bsp_touch_init());

    ESP_ERROR_CHECK(input_init(INPUT_BACKEND_TOUCH_ZONES));

    app_registry_scan();   /* returns NOT_FOUND without a card; not fatal */

    ESP_ERROR_CHECK(shell_start(io, panel));

    ESP_LOGI(TAG, "boot complete, free heap %lu", (unsigned long)esp_get_free_heap_size());

#if DESKOS_CHORD_SELFTEST
    xTaskCreatePinnedToCore(chord_selftest, "chordtest", 3072, NULL, 3, NULL, 0);
#endif
}
