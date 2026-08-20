#include "app_registry.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bsp.h"
#include "cJSON.h"
#include "esp_log.h"
#include "input.h"

static const char *TAG = "registry";

static app_info_t s_apps[APP_REGISTRY_MAX];
static size_t     s_count;

typedef struct {
    char dir_name[32];
    char reason[48];
} app_scan_error_t;

static app_scan_error_t s_errors[APP_REGISTRY_MAX_ERRORS];
static size_t           s_error_count;

static void record_error(const char *dir_name, const char *reason)
{
    if (s_error_count >= APP_REGISTRY_MAX_ERRORS) {
        ESP_LOGW(TAG, "error list full, dropping: %s: %s", dir_name, reason);
        return;
    }
    app_scan_error_t *e = &s_errors[s_error_count++];
    snprintf(e->dir_name, sizeof(e->dir_name), "%s", dir_name);
    snprintf(e->reason, sizeof(e->reason), "%s", reason);
}

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

/* "bindings": {"B2 long": "refresh"}. Layer A has no code, so the value must
 * name one of the runtime's own fixed actions, not an arbitrary handler — see
 * app_action_t. A binding that will never fire because it collides with a
 * system chord is not rejected here: shell.c gives its own chords first
 * refusal on every event, so the collision resolves itself at dispatch time
 * without a second reserved-combo list to keep in sync with this one. */
static void parse_bindings(const cJSON *obj, app_info_t *out)
{
    if (!cJSON_IsObject(obj)) {
        return;
    }

    const cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, obj)
    {
        if (out->nbindings >= APP_MAX_BINDINGS) {
            ESP_LOGW(TAG, "%s: too many bindings (max %d) — rest ignored",
                     out->id, APP_MAX_BINDINGS);
            break;
        }

        input_event_t ev;
        if (!input_parse_event_str(entry->string, &ev)) {
            ESP_LOGW(TAG, "%s: bad binding key '%s' — ignored", out->id, entry->string);
            continue;
        }

        const char *action_str = cJSON_GetStringValue(entry);
        app_action_t action = APP_ACTION_NONE;
        if (action_str && strcmp(action_str, "refresh") == 0) {
            action = APP_ACTION_REFRESH;
        } else {
            ESP_LOGW(TAG, "%s: unknown binding action '%s' for '%s' — ignored",
                     out->id, action_str ? action_str : "?", entry->string);
            continue;
        }

        out->bindings[out->nbindings].ev = ev;
        out->bindings[out->nbindings].action = action;
        out->nbindings++;
    }
}

static void copy_str(char *dst, size_t dst_sz, const cJSON *node, const char *fallback)
{
    const char *s = cJSON_GetStringValue(node);
    if (!s || !*s) {
        s = fallback;
    }
    snprintf(dst, dst_sz, "%s", s);
}

static bool load_manifest(const char *root_dir, const char *dir_name, app_info_t *out)
{
    char path[160];
    snprintf(path, sizeof(path), "%s/%s/manifest.json", root_dir, dir_name);

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
        record_error(dir_name, "empty manifest");
        return false;
    }
    buf[n] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        ESP_LOGW(TAG, "%s: manifest is not valid JSON", dir_name);
        record_error(dir_name, "invalid JSON");
        return false;
    }

    bool ok = false;
    const int api = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(root, "api"));
    if (api != APP_MANIFEST_API_VER) {
        /* Refusing an unknown API version is the whole point of having one:
         * a future manifest must not be half-interpreted by an old shell. */
        ESP_LOGW(TAG, "%s: manifest api %d, shell speaks %d — skipped",
                 dir_name, api, APP_MANIFEST_API_VER);
        char reason[48];
        snprintf(reason, sizeof(reason), "api %d, shell speaks %d", api, APP_MANIFEST_API_VER);
        record_error(dir_name, reason);
        goto done;
    }

    memset(out, 0, sizeof(*out));
    out->api = api;
    copy_str(out->id,      sizeof(out->id),      cJSON_GetObjectItem(root, "id"),      dir_name);
    copy_str(out->name,    sizeof(out->name),    cJSON_GetObjectItem(root, "name"),    dir_name);
    copy_str(out->version, sizeof(out->version), cJSON_GetObjectItem(root, "version"), "0.0.0");
    copy_str(out->entry,   sizeof(out->entry),   cJSON_GetObjectItem(root, "entry"),   "ui.jsonl");
    snprintf(out->dir, sizeof(out->dir), "%s/%s", root_dir, dir_name);

    const char *layer = cJSON_GetStringValue(cJSON_GetObjectItem(root, "layer"));
    out->layer = (layer && strcmp(layer, "lua") == 0) ? APP_LAYER_LUA : APP_LAYER_DECLARATIVE;

    out->caps = parse_caps(cJSON_GetObjectItem(root, "capabilities"));
    parse_bindings(cJSON_GetObjectItem(root, "bindings"), out);
    ok = true;

done:
    cJSON_Delete(root);
    return ok;
}

static void scan_root(const char *root_dir)
{
    DIR *d = opendir(root_dir);
    if (!d) {
        ESP_LOGD(TAG, "%s absent", root_dir);
        return;
    }

    const struct dirent *ent = NULL;
    while ((ent = readdir(d)) != NULL && s_count < APP_REGISTRY_MAX) {
        if (ent->d_name[0] == '.') {
            continue;
        }
        if (load_manifest(root_dir, ent->d_name, &s_apps[s_count])) {
            ESP_LOGI(TAG, "app '%s' v%s (%s) from %s", s_apps[s_count].id,
                     s_apps[s_count].version,
                     s_apps[s_count].layer == APP_LAYER_LUA ? "lua" : "declarative",
                     root_dir);
            s_count++;
        }
    }
    closedir(d);
}

esp_err_t app_registry_scan(void)
{
    s_count = 0;
    s_error_count = 0;

    /* Built-ins live in internal flash so the device is useful with no card at
     * all; the card is scanned second and simply adds to the list. Both go
     * through the same filesystem path, so there is only one loader to trust. */
    scan_root(BSP_FS_MOUNT_POINT "/apps");

    if (bsp_sd_is_mounted()) {
        scan_root(BSP_SD_MOUNT_POINT "/apps");
    } else {
        ESP_LOGW(TAG, "no microSD — built-in apps only");
    }

    ESP_LOGI(TAG, "%u app(s) registered", (unsigned)s_count);
    return (s_count > 0) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

size_t app_registry_count(void)
{
    return s_count;
}

const app_info_t *app_registry_get(size_t index)
{
    return (index < s_count) ? &s_apps[index] : NULL;
}

app_action_t app_registry_action_for(const app_info_t *app, const input_event_t *ev)
{
    if (!app || !ev) {
        return APP_ACTION_NONE;
    }
    for (size_t i = 0; i < app->nbindings; i++) {
        if (app->bindings[i].ev.mask == ev->mask && app->bindings[i].ev.kind == ev->kind) {
            return app->bindings[i].action;
        }
    }
    return APP_ACTION_NONE;
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

size_t app_registry_error_count(void)
{
    return s_error_count;
}

bool app_registry_get_error(size_t index, char *dir_out, size_t dir_sz,
                             char *reason_out, size_t reason_sz)
{
    if (index >= s_error_count) {
        return false;
    }
    snprintf(dir_out, dir_sz, "%s", s_errors[index].dir_name);
    snprintf(reason_out, reason_sz, "%s", s_errors[index].reason);
    return true;
}

esp_err_t app_registry_delete(const char *app_id)
{
    const app_info_t *app = NULL;
    for (size_t i = 0; i < s_count; i++) {
        if (strcmp(s_apps[i].id, app_id) == 0) {
            app = &s_apps[i];
            break;
        }
    }
    if (!app) {
        return ESP_ERR_NOT_FOUND;
    }
    /* Built-ins are re-extracted from firmware at every boot (see main.c) —
     * deleting one would just be undone on the next reboot, so refuse rather
     * than silently do nothing useful. */
    if (strncmp(app->dir, BSP_SD_MOUNT_POINT, strlen(BSP_SD_MOUNT_POINT)) != 0) {
        ESP_LOGW(TAG, "%s: refusing to delete a built-in app (%s)", app_id, app->dir);
        return ESP_ERR_INVALID_ARG;
    }

    DIR *d = opendir(app->dir);
    if (!d) {
        return ESP_FAIL;
    }
    const struct dirent *ent = NULL;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') {
            continue;
        }
        char path[350];
        snprintf(path, sizeof(path), "%s/%s", app->dir, ent->d_name);
        struct stat st;
        /* App directories are documented flat (manifest.json, the entry
         * file, .cache.json) — no real case makes a subdirectory, so this is
         * a defensive skip, not a recursive delete. */
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
            ESP_LOGW(TAG, "%s: skipping unexpected subdirectory '%s'", app_id, ent->d_name);
            continue;
        }
        if (unlink(path) != 0) {
            ESP_LOGW(TAG, "%s: failed to remove '%s'", app_id, path);
        }
    }
    closedir(d);

    if (rmdir(app->dir) != 0) {
        ESP_LOGW(TAG, "%s: rmdir '%s' failed (not empty?)", app_id, app->dir);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "%s: deleted from %s", app_id, app->dir);
    return ESP_OK;
}
