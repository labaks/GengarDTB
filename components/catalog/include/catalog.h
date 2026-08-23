/*
 * App catalog (ROADMAP #20) — a list of installable apps by URL, downloaded
 * to the SD card through the same primitive fetch.c already uses for
 * pushing icon batches (ROADMAP #34 follow-up): a small manifest naming
 * URL -> destination pairs.
 *
 * Two documents, both plain JSON over HTTP, same bootstrap-pointer shape as
 * every other SD config file in this project (/sd/wifi.json, /sd/ota.json,
 * /sd/fetch.json):
 *
 *   /sd/catalog.json is either {"apps":[...]} directly, or (so the card
 *   never has to be touched again after the first time) {"catalog_url":
 *   "http://host/catalog.json"} pointing at a same-shaped remote document.
 *
 *   Each app entry's own "manifest_url" points at a fetch.h-shaped
 *   {"items":[{"url":...,"dest":...}, ...]} document — the exact same
 *   format fetch_run() already downloads from /sd/fetch.json, just per-app
 *   instead of one fixed file. Installing an app is nothing more than
 *   fetching that document and running fetch_items() on it into
 *   /sd/apps/<id>/.
 *
 * Version/api compatibility is not this file's job: app_registry_scan()
 * already rejects a manifest.json whose "api" does not match
 * APP_MANIFEST_API_VER (see app_registry.h) and reports it the same way a
 * hand-copied bad app would. catalog.c only decides what to fetch; the
 * registry is still the one authority on whether the result is valid.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "fetch.h"   /* fetch_progress_cb_t/fetch_state_t, reused for install progress */

#define CATALOG_MAX_APPS 16

typedef struct {
    char id[24];
    char name[40];
    char version[16];
    char manifest_url[192];
} catalog_app_t;

typedef enum {
    CATALOG_CHECKING,
    CATALOG_DONE,
    CATALOG_ERROR,   /* detail holds a short reason */
} catalog_check_state_t;

typedef void (*catalog_check_cb_t)(catalog_check_state_t state, const char *detail);

/* Reads /sd/catalog.json, resolves "catalog_url" if present, and replaces
 * the in-memory app list with whatever "apps" it finds. Runs in its own
 * background task — cb is called from that task, marshal onto the LVGL
 * thread yourself (same convention as fetch_progress_cb_t). A no-op if a
 * check is already running. Entries beyond CATALOG_MAX_APPS are silently
 * dropped, not an error — a catalog that large is not this device's use
 * case, but truncating rather than failing keeps the first N usable. */
void catalog_check(catalog_check_cb_t cb);

bool catalog_check_is_running(void);

size_t catalog_count(void);

/* NULL if index is out of range. The pointer is only valid until the next
 * catalog_check() — copy out what you need before calling it again. */
const catalog_app_t *catalog_get(size_t index);

/* Fetches catalog_get(index)->manifest_url and downloads every item in it
 * to disk via fetch_items() — same reporting as fetch_run() itself, reused
 * as-is rather than inventing a parallel enum for what is the same
 * operation with a different item list. Runs in its own background task. A
 * no-op if an install is already running (this one or fetch_run()'s own —
 * they write to the same card and there's no reason to interleave). Caller
 * must still rescan the registry and refresh the app list afterward, same
 * as every other install/delete path in this project. */
void catalog_install(size_t index, fetch_progress_cb_t cb);

bool catalog_install_is_running(void);

#ifdef __cplusplus
}
#endif
