#include "catalog.h"

#include <stdio.h>
#include <string.h>

#include "bsp_pins.h"
#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "catalog";

#define CATALOG_LOCAL_MAX_BODY 512   /* just a bootstrap pointer or a tiny inline list */

static catalog_app_t s_apps[CATALOG_MAX_APPS];
static size_t        s_napps;
static volatile bool s_checking;
static volatile bool s_installing;

/* {"apps":[...]} or {"catalog_url":"..."} on the card — same convention as
 * /sd/fetch.json's own "manifest_url". */
static cJSON *load_local_catalog(void)
{
    char path[64];
    snprintf(path, sizeof(path), "%s/catalog.json", BSP_SD_MOUNT_POINT);

    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    char buf[CATALOG_LOCAL_MAX_BODY];
    const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    return cJSON_Parse(buf);
}

/* Resolves the local file down to the document actually worth iterating:
 * either it already has "apps", or it points at "catalog_url" and this
 * fetches that instead — same shape as fetch.c's own load_manifest(). */
static cJSON *load_catalog_doc(void)
{
    cJSON *root = load_local_catalog();
    if (!root) {
        return NULL;
    }
    const char *catalog_url = cJSON_GetStringValue(cJSON_GetObjectItem(root, "catalog_url"));
    if (catalog_url && *catalog_url) {
        cJSON *remote = fetch_json_url(catalog_url);
        cJSON_Delete(root);
        return remote;
    }
    return root;
}

static void catalog_check_task(void *arg)
{
    const catalog_check_cb_t cb = (catalog_check_cb_t)arg;

    cJSON *root = load_catalog_doc();
    const cJSON *apps = root ? cJSON_GetObjectItem(root, "apps") : NULL;
    if (!cJSON_IsArray(apps)) {
        ESP_LOGW(TAG, "no /sd/catalog.json (or no 'apps' array)");
        s_napps = 0;
        if (cb) {
            cb(CATALOG_ERROR, "no /sd/catalog.json");
        }
        cJSON_Delete(root);
        goto done;
    }

    {
        size_t n = 0;
        const cJSON *app;
        cJSON_ArrayForEach(app, apps) {
            if (n >= CATALOG_MAX_APPS) {
                ESP_LOGW(TAG, "catalog has more than %d apps — rest ignored", CATALOG_MAX_APPS);
                break;
            }
            const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(app, "id"));
            const char *manifest_url = cJSON_GetStringValue(cJSON_GetObjectItem(app, "manifest_url"));
            if (!id || !*id || !manifest_url || !*manifest_url) {
                continue;   /* a malformed entry is skipped, not a reason to fail the whole list */
            }
            catalog_app_t *out = &s_apps[n];
            snprintf(out->id, sizeof(out->id), "%s", id);
            const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(app, "name"));
            snprintf(out->name, sizeof(out->name), "%s", (name && *name) ? name : id);
            const char *version = cJSON_GetStringValue(cJSON_GetObjectItem(app, "version"));
            snprintf(out->version, sizeof(out->version), "%s", version ? version : "?");
            snprintf(out->manifest_url, sizeof(out->manifest_url), "%s", manifest_url);
            n++;
        }
        s_napps = n;
        cJSON_Delete(root);

        ESP_LOGI(TAG, "catalog: %u app(s)", (unsigned)s_napps);
        if (cb) {
            cb(CATALOG_DONE, NULL);
        }
    }

done:
    s_checking = false;
    vTaskDelete(NULL);
}

void catalog_check(catalog_check_cb_t cb)
{
    if (s_checking) {
        return;
    }
    s_checking = true;
    /* Fired synchronously, on the caller's own task, before the background
     * task even starts — callers only ever call this from a button click on
     * the LVGL task, same as ota_check()'s "checking" report, so there is no
     * lock to take here (that is only needed for the DONE/ERROR report from
     * catalog_check_task's own task). */
    if (cb) {
        cb(CATALOG_CHECKING, NULL);
    }
    if (xTaskCreate(catalog_check_task, "catalog_chk", 8192, (void *)cb, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "cannot start catalog check task");
        s_checking = false;
    }
}

bool catalog_check_is_running(void)
{
    return s_checking;
}

size_t catalog_count(void)
{
    return s_napps;
}

const catalog_app_t *catalog_get(size_t index)
{
    if (index >= s_napps) {
        return NULL;
    }
    return &s_apps[index];
}

typedef struct {
    char                 manifest_url[192];
    fetch_progress_cb_t  cb;
} install_args_t;

static void catalog_install_task(void *arg)
{
    install_args_t *args = arg;
    const fetch_progress_cb_t cb = args->cb;

    if (cb) {
        cb(FETCH_CONNECTING, 0, args->manifest_url);
    }
    cJSON *root = fetch_json_url(args->manifest_url);
    const cJSON *items = root ? cJSON_GetObjectItem(root, "items") : NULL;
    if (!cJSON_IsArray(items)) {
        ESP_LOGE(TAG, "app manifest '%s' has no 'items' array", args->manifest_url);
        if (cb) {
            cb(FETCH_ERROR, 0, "bad app manifest");
        }
        cJSON_Delete(root);
        goto done;
    }

    {
        const bool ok = fetch_items(items, cb);
        cJSON_Delete(root);
        if (cb) {
            cb(ok ? FETCH_DONE : FETCH_ERROR, 100, ok ? NULL : "some files failed");
        }
    }

done:
    free(args);
    s_installing = false;
    vTaskDelete(NULL);
}

void catalog_install(size_t index, fetch_progress_cb_t cb)
{
    if (s_installing || fetch_is_running()) {
        return;
    }
    const catalog_app_t *app = catalog_get(index);
    if (!app) {
        return;
    }

    install_args_t *args = malloc(sizeof(*args));
    if (!args) {
        return;
    }
    snprintf(args->manifest_url, sizeof(args->manifest_url), "%s", app->manifest_url);
    args->cb = cb;

    s_installing = true;
    if (xTaskCreate(catalog_install_task, "catalog_inst", 8192, args, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "cannot start catalog install task");
        free(args);
        s_installing = false;
    }
}

bool catalog_install_is_running(void)
{
    return s_installing;
}
