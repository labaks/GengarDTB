/*
 * Layer A widget runtime.
 *
 * A widget is a ui.jsonl file: one JSON object per line, either a view or the
 * data source that feeds it. Nothing from the app is executed — the device only
 * lays out views and substitutes values fetched from a URL. That is what makes
 * "the user installs a widget by copying a folder" affordable here.
 */
#pragma once

#include <stdbool.h>
#include "app_registry.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Builds the widget's screen from <app->dir>/<app->entry> and shows it.
 * Starts the refresh task when the file declares an http source.
 * Must be called with the LVGL lock held. */
esp_err_t widget_open(const app_info_t *app);

/* Stops refreshing, restores the previous screen and frees everything.
 * Safe to call when nothing is open. Must be called with the LVGL lock held. */
void widget_close(void);

bool widget_is_open(void);

/* Widget currently on screen, or NULL. */
const app_info_t *widget_current(void);

#ifdef __cplusplus
}
#endif
