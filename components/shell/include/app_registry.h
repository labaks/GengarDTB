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

#ifdef __cplusplus
extern "C" {
#endif

#define APP_REGISTRY_MAX      24
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

typedef struct {
    char        id[24];
    char        name[40];
    char        version[16];
    char        entry[32];
    char        dir[80];
    app_layer_t layer;
    uint32_t    caps;
    int         api;
} app_info_t;

/* Rescans the card. Safe to call with no card inserted: yields zero apps and
 * returns ESP_ERR_NOT_FOUND rather than failing the boot. */
esp_err_t app_registry_scan(void);

size_t              app_registry_count(void);
const app_info_t   *app_registry_get(size_t index);

/* True when the app can do anything useful right now. An app declaring
 * APP_CAP_HOST is "degraded", not hidden, while the PC agent is away. */
bool app_registry_is_available(const app_info_t *app, bool host_connected);

#ifdef __cplusplus
}
#endif
