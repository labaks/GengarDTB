/*
 * Push files onto the microSD card over WiFi (ROADMAP #34 follow-up).
 *
 * Nothing in this project ever wrote to the card over the network before —
 * every SD-resident config file (/sd/wifi.json, /sd/agent.json,
 * /sd/ota.json, /sd/device.json) and every app/asset on it got there by
 * physically moving the card to a reader. That is fine for a one-time setup
 * file, but is the wrong way to deliver a batch of icons (or, later, a
 * downloaded app for ROADMAP #20's catalog) every time one changes. This is
 * the actual mechanism: a small manifest naming URL -> destination pairs,
 * fetched over plain HTTP and written straight to the card.
 */
#pragma once

#include <stdbool.h>
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FETCH_CONNECTING,    /* opening the current item's HTTP connection */
    FETCH_DOWNLOADING,   /* percent = items completed so far * 100 / total; detail = current dest */
    FETCH_DONE,
    FETCH_ERROR,         /* detail holds a short reason; the item that failed is left alone */
} fetch_state_t;

typedef void (*fetch_progress_cb_t)(fetch_state_t state, int percent, const char *detail);

/* Reads /sd/fetch.json, which is either:
 *   {"items":[{"url":"http://host/a.bin","dest":"/sd/icons/weather/a.bin"}, ...]}
 * or, so the card never has to be touched again after the first time:
 *   {"manifest_url":"http://host/fetch_all.json"}
 * — a pointer to a same-shaped {"items":[...]} document fetched fresh over
 * HTTP on every run. The list that actually changes from batch to batch
 * lives on the PC; the one-time file on the card is just a bootstrap
 * pointer, same spirit as /sd/agent.json holding a token rather than the
 * whole protocol. Either way, each item is downloaded straight to its dest
 * path in a background task — cb is called from that task, marshal onto the
 * LVGL thread yourself before touching any LVGL object (same convention as
 * ota_progress_cb_t). Missing parent directories under dest are created as
 * needed. One item failing does not stop the rest. A no-op if a fetch is
 * already running. */
void fetch_run(fetch_progress_cb_t cb);

bool fetch_is_running(void);

/* Blocking GET of a small JSON document (10s timeout, 8 KB cap, no auto
 * redirect) — the exact primitive /sd/fetch.json's own "manifest_url"
 * indirection already used internally to fetch a remote {"items":[...]}
 * document. Exposed for catalog.c (ROADMAP #20), which needs the same "get
 * me a JSON manifest from a URL" step for both the app catalog itself and
 * each app's own file list. NULL on any failure (bad URL, timeout, non-200,
 * malformed JSON, over the size cap). Caller owns the result. Must not run
 * on the LVGL task — same rule as datasource_fetch_json(). */
cJSON *fetch_json_url(const char *url);

/* Downloads every {"url":...,"dest":...} pair in items to its dest path,
 * one at a time, creating missing parent directories as needed — the same
 * per-item behavior and cb reporting fetch_run() itself uses once it has
 * resolved /sd/fetch.json down to an items array. Runs synchronously on the
 * calling task; fetch_run() calls this from its own background task, and a
 * caller with its own task (catalog.c's install path) can call it directly.
 * items is not modified or freed. Returns true iff every item succeeded. */
bool fetch_items(const cJSON *items, fetch_progress_cb_t cb);

#ifdef __cplusplus
}
#endif
