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

/* Reads /sd/fetch.json:
 *   {"items":[{"url":"http://host/a.bin","dest":"/sd/icons/weather/a.bin"}, ...]}
 * and downloads each item straight to its dest path, in a background task —
 * cb is called from that task, marshal onto the LVGL thread yourself before
 * touching any LVGL object (same convention as ota_progress_cb_t). Missing
 * parent directories under dest are created as needed. One item failing
 * does not stop the rest. A no-op if a fetch is already running. */
void fetch_run(fetch_progress_cb_t cb);

bool fetch_is_running(void);

#ifdef __cplusplus
}
#endif
