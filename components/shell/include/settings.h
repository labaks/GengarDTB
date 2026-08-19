/*
 * Native settings screen — not a ui.jsonl widget. Brightness, timezone, WiFi/SD
 * status, free heap, firmware version, and the checklist of apps pinned to
 * Home (see docs/shell-navigation.md — pinning only happens here, not via a
 * button chord on the app list).
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Builds and shows the settings screen. Must be called with the LVGL lock
 * held (same convention as widget_open). A no-op if already open. */
esp_err_t settings_open(void);

/* Restores the previous screen and frees everything. Safe to call when
 * nothing is open. Must be called with the LVGL lock held. */
void settings_close(void);

bool settings_is_open(void);

/* True when app_id is in the pinned-to-Home set. Reads NVS directly, so it
 * is safe to call whether or not the settings screen is currently open —
 * Home (ROADMAP #19, not built yet) will call this to pick what to show. */
bool settings_app_is_pinned(const char *app_id);

#ifdef __cplusplus
}
#endif
