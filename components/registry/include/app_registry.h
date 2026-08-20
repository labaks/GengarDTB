/*
 * Widget app registry — scans /sd/apps/<id>/manifest.json.
 *
 * Apps deliberately live on the microSD card, not in flash: this board has 4 MB
 * total and dual OTA slots consume almost all of it. See CLAUDE.md.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "input.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_REGISTRY_MAX      24
#define APP_REGISTRY_MAX_ERRORS 8
#define APP_MANIFEST_API_VER  1

/* Capabilities an app must declare up front. The shell uses these to decide what
 * the app may touch and, crucially, how it behaves when the PC is asleep. */
#define APP_CAP_NET_HTTP  (1u << 0)
#define APP_CAP_TIME      (1u << 1)
#define APP_CAP_STORAGE   (1u << 2)
#define APP_CAP_HOST      (1u << 3)   /* needs the PC agent -> may be unavailable */
#define APP_CAP_NOTIFY    (1u << 4)   /* RGB LED + speaker */

typedef enum {
    APP_LAYER_DECLARATIVE,  /* ui.jsonl + a data source. No code runs on device. */
    APP_LAYER_LUA,          /* main.lua. Not implemented yet — layer B. */
} app_layer_t;

/* Layer A has no code, so a "binding" can only name one of a small, fixed
 * set of actions the runtime itself knows how to perform — not an arbitrary
 * app-defined handler. refresh is the only one so far (ROADMAP #16): force
 * a widget's http sources to re-fetch now instead of waiting out `every`. */
typedef enum {
    APP_ACTION_NONE = 0,
    APP_ACTION_REFRESH,
} app_action_t;

#define APP_MAX_BINDINGS 4

typedef struct {
    input_event_t ev;
    app_action_t  action;
} app_binding_t;

typedef struct {
    char          id[24];
    char          name[40];
    char          version[16];
    char          entry[32];
    char          dir[80];
    app_layer_t   layer;
    uint32_t      caps;
    int           api;
    app_binding_t bindings[APP_MAX_BINDINGS];
    size_t        nbindings;
} app_info_t;

/* Looks up the action bound to `ev` in app's manifest, or APP_ACTION_NONE.
 * The shell calls this only after its own system chords have already had
 * first refusal — see shell.c — so a binding that collides with a system
 * combo simply never reaches here, no separate conflict table needed. */
app_action_t app_registry_action_for(const app_info_t *app, const input_event_t *ev);

/* Rescans the card. Safe to call with no card inserted: yields zero apps and
 * returns ESP_ERR_NOT_FOUND rather than failing the boot. Also safe to call
 * again later, not just at startup (ROADMAP #18) — resets and repopulates
 * both this list and the error list below. */
esp_err_t app_registry_scan(void);

size_t              app_registry_count(void);
const app_info_t   *app_registry_get(size_t index);

/* A manifest.json that existed but failed to load — invalid JSON, empty
 * file, unrecognized api version. A directory with no manifest.json at all
 * is not an error (could be anything — a stray folder, work in progress —
 * so it is silently skipped, not recorded here). Reset on every
 * app_registry_scan(). */
size_t app_registry_error_count(void);

/* dir_out/reason_out are always null-terminated. Returns false (and leaves
 * both untouched) if index is out of range. */
bool app_registry_get_error(size_t index, char *dir_out, size_t dir_sz,
                             char *reason_out, size_t reason_sz);

/* Deletes an SD-card app's entire directory (manifest.json, its entry file,
 * any .cache.json) permanently — no confirmation, no undo, that is the
 * caller's job. Refuses a built-in app (lives under BSP_FS_MOUNT_POINT,
 * re-extracted from firmware at every boot regardless — deleting it would
 * just be undone on the next reboot). Does not rescan; call
 * app_registry_scan() afterward to make the change visible. */
esp_err_t app_registry_delete(const char *app_id);

/* True when the app can do anything useful right now. An app declaring
 * APP_CAP_HOST is "degraded", not hidden, while the PC agent is away. */
bool app_registry_is_available(const app_info_t *app, bool host_connected);

#ifdef __cplusplus
}
#endif
