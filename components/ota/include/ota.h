/*
 * OTA firmware update over WiFi (ROADMAP #17, #38.4).
 *
 * The partition table has carried a dual OTA slot + rollback layout since
 * day one (see CLAUDE.md), but nothing ever wrote to the spare slot — every
 * update this whole project has had so far went in over USB via esptool.
 * This is the actual mechanism.
 *
 * Two separate entry points on purpose (#38.4): ota_check() only ever reads
 * a small JSON manifest over plain HTTP — it never touches
 * esp_https_ota/esp_ota_ops at all, so it cannot erase or write a single
 * flash sector. ota_start_update() is the one that actually does that, and
 * is meant to run only after a human has seen what ota_check() found and
 * pressed a second, separate button.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OTA_CHECK_CHECKING,     /* fetching the manifest */
    OTA_CHECK_AVAILABLE,    /* version/size_bytes valid, newer than the running image */
    OTA_CHECK_UP_TO_DATE,   /* version valid, but not newer (SemVer compare) */
    OTA_CHECK_ERROR,        /* detail holds a short reason */
} ota_check_state_t;

typedef void (*ota_check_cb_t)(ota_check_state_t state, const char *version, size_t size_bytes,
                               const char *detail);

/* Reads /sd/ota.json's "manifest_url" and GETs that URL — a plain JSON
 * document, {"version":"1.2.3","size":123456,"notes":"..."}, "size" and
 * "notes" optional. Compares "version" against this build's own version
 * (esp_app_get_description()->version, ROADMAP #38.3) with a plain X.Y.Z
 * SemVer comparison — no pre-release/build-metadata suffixes expected or
 * handled. Runs in a background task; cb is called from that task, marshal
 * onto the LVGL thread yourself before touching any LVGL object. A no-op if
 * a check or an update is already running. */
void ota_check(ota_check_cb_t cb);

typedef enum {
    OTA_CONNECTING,     /* opening the HTTP(S) connection */
    OTA_DOWNLOADING,    /* percent valid (0-100), detail may hold the new version once known */
    OTA_DONE,           /* image written and verified; rebooting shortly */
    OTA_ERROR,          /* detail holds a short reason */
} ota_state_t;

typedef void (*ota_progress_cb_t)(ota_state_t state, int percent, const char *detail);

/* Reads /sd/ota.json's "url" and downloads and flashes that image into the
 * inactive OTA slot in a background task, reporting progress through cb
 * (same marshalling caveat as ota_check_cb_t above). On success the device
 * reboots into the new image on its own; on failure the currently running
 * firmware is untouched. Meant to run only after ota_check() has shown the
 * user what is available and they have confirmed — this function itself
 * does not check anything, it just updates. A no-op if a check or an
 * update is already running. */
void ota_start_update(ota_progress_cb_t cb);

bool ota_is_running(void);

#ifdef __cplusplus
}
#endif
