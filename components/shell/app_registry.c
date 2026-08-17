#include "app_registry.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "bsp.h"
#include "cJSON.h"
#include "esp_log.h"

static const char *TAG = "registry";

static app_info_t s_apps[APP_REGISTRY_MAX];
static size_t     s_count;

static uint32_t parse_caps(const cJSON *arr)
{
    uint32_t caps = 0;
    if (!cJSON_IsArray(arr)) {
        return 0;
    }

    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, arr) {
        const char *s = cJSON_GetStringValue(item);
        if (!s) {
            continue;
        }
        if      (strcmp(s, "net.http") == 0) { caps |= APP_CAP_NET_HTTP; }
        else if (strcmp(s, "time")     == 0) { caps |= APP_CAP_TIME; }
        else if (strcmp(s, "storage")  == 0) { caps |= APP_CAP_STORAGE; }
        else if (strcmp(s, "host")     == 0) { caps |= APP_CAP_HOST; }
        else if (strcmp(s, "notify")   == 0) { caps |= APP_CAP_NOTIFY; }
        else { ESP_LOGW(TAG, "unknown capability '%s' — ignored", s); }
    }
    return caps;
}

static void copy_str(char *dst, size_t dst_sz, const cJSON *node, const char *fallback)
{
    const char *s = cJSON_GetStringValue(node);
    if (!s || !*s) {
        s = fallback;
    }
    snprintf(dst, dst_sz, "%s", s);
}

static bool load_manifest(const char *dir_name, app_info_t *out)
{
    char path[128];
    snprintf(path, sizeof(path), "%s/apps/%s/manifest.json", BSP_SD_MOUNT_POINT, dir_name);

    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }

    /* Manifests are tiny by contract. Anything larger is malformed, and we are not
     * going to spend precious heap finding out. */
    char buf[1024];
    const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0) {
        ESP_LOGW(TAG, "%s: empty manifest", dir_name);
        return false;
    }
    buf[n] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        ESP_LOGW(TAG, "%s: manifest is not valid JSON", dir_name);
        return false;
    }

    bool ok = false;
    const int api = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(root, "api"));
    if (api != APP_MANIFEST_API_VER) {
        /* Refusing an unknown API version is the whole point of having one:
         * a future manifest must not be half-interpreted by an old shell. */
        ESP_LOGW(TAG, "%s: manifest api %d, shell speaks %d — skipped",
                 dir_name, api, APP_MANIFEST_API_VER);
        goto done;
    }

    memset(out, 0, sizeof(*out));
    out->api = api;
    copy_str(out->id,      sizeof(out->id),      cJSON_GetObjectItem(root, "id"),      dir_name);
    copy_str(out->name,    sizeof(out->name),    cJSON_GetObjectItem(root, "name"),    dir_name);
    copy_str(out->version, sizeof(out->version), cJSON_GetObjectItem(root, "version"), "0.0.0");
    copy_str(out->entry,   sizeof(out->entry),   cJSON_GetObjectItem(root, "entry"),   "ui.jsonl");
    snprintf(out->dir, sizeof(out->dir), "%s/apps/%s", BSP_SD_MOUNT_POINT, dir_name);

    const char *layer = cJSON_GetStringValue(cJSON_GetObjectItem(root, "layer"));
    out->layer = (layer && strcmp(layer, "lua") == 0) ? APP_LAYER_LUA : APP_LAYER_DECLARATIVE;

    out->caps = parse_caps(cJSON_GetObjectItem(root, "capabilities"));
    ok = true;

done:
    cJSON_Delete(root);
    return ok;
}

esp_err_t app_registry_scan(void)
{
    s_count = 0;

    if (!bsp_sd_is_mounted()) {
        ESP_LOGW(TAG, "no microSD — zero apps, shell continues");
        return ESP_ERR_NOT_FOUND;
    }

    char apps_dir[64];
    snprintf(apps_dir, sizeof(apps_dir), "%s/apps", BSP_SD_MOUNT_POINT);

    DIR *d = opendir(apps_dir);
    if (!d) {
        ESP_LOGW(TAG, "%s missing — create it and drop app folders in", apps_dir);
        return ESP_ERR_NOT_FOUND;
    }

    const struct dirent *ent = NULL;
    while ((ent = readdir(d)) != NULL && s_count < APP_REGISTRY_MAX) {
        if (ent->d_name[0] == '.') {
            continue;
        }
        if (load_manifest(ent->d_name, &s_apps[s_count])) {
            ESP_LOGI(TAG, "app '%s' v%s (%s)", s_apps[s_count].id,
                     s_apps[s_count].version,
                     s_apps[s_count].layer == APP_LAYER_LUA ? "lua" : "declarative");
            s_count++;
        }
    }
    closedir(d);

    ESP_LOGI(TAG, "%u app(s) registered", (unsigned)s_count);
    return ESP_OK;
}

size_t app_registry_count(void)
{
    return s_count;
}

const app_info_t *app_registry_get(size_t index)
{
    return (index < s_count) ? &s_apps[index] : NULL;
}

bool app_registry_is_available(const app_info_t *app, bool host_connected)
{
    if (!app) {
        return false;
    }
    /* Layer B is not implemented yet, so a Lua app is listed but not runnable. */
    if (app->layer == APP_LAYER_LUA) {
        return false;
    }
    if (app->caps & APP_CAP_HOST) {
        return host_connected;
    }
    return true;
}
