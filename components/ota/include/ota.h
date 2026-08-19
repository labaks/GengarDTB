/*
 * OTA firmware update over WiFi (ROADMAP #17).
 *
 * The partition table has carried a dual OTA slot + rollback layout since
 * day one (see CLAUDE.md), but nothing ever wrote to the spare slot — every
 * update this whole project has had so far went in over USB via esptool.
 * This is the actual mechanism.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OTA_CONNECTING,     /* opening the HTTP(S) connection */
    OTA_DOWNLOADING,    /* percent valid (0-100), detail may hold the new version once known */
    OTA_DONE,           /* image written and verified; rebooting shortly */
    OTA_ERROR,          /* detail holds a short reason */
} ota_state_t;

typedef void (*ota_progress_cb_t)(ota_state_t state, int percent, const char *detail);

/* Reads /sd/ota.json ({"url":"http://host/deskos.bin"}) and, if present,
 * downloads and flashes that image into the inactive OTA slot in a
 * background task, reporting progress through cb (called from that task —
 * marshal onto the LVGL thread yourself before touching any LVGL object).
 * On success the device reboots into the new image on its own; on failure
 * the currently running firmware is untouched. A no-op if an update is
 * already in progress. */
void ota_check_and_update(ota_progress_cb_t cb);

bool ota_is_running(void);

#ifdef __cplusplus
}
#endif
