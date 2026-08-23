#include "fetch.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "bsp_pins.h"
#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "fetch";

#define FETCH_MANIFEST_MAX_BODY 8192

static volatile bool s_running;

/* Creates every missing directory in path's parent chain (e.g. for
 * "/sd/icons/weather/a.bin" that's /sd, /sd/icons, /sd/icons/weather).
 * FATFS's mkdir() has no -p option and errors on an existing directory —
 * both are fine here, only a genuine failure to create a missing one
 * matters. path is modified in place and restored before returning. */
static void mkdir_p_parent(char *path)
{
    for (char *p = path + 1; *p; p++) {
        if (*p != '/') {
            continue;
        }
        *p = '\0';
        mkdir(path, 0777);   /* ignore EEXIST and everything else — the
                               * fopen() right after this is the real check */
        *p = '/';
    }
}

typedef struct {
    FILE  *file;
    size_t written;
} sink_t;

static esp_err_t http_event(esp_http_client_event_t *evt)
{
    sink_t *sink = evt->user_data;
    if (evt->event_id != HTTP_EVENT_ON_DATA || !sink || !sink->file) {
        return ESP_OK;
    }
    if (fwrite(evt->data, 1, evt->data_len, sink->file) != (size_t)evt->data_len) {
        ESP_LOGE(TAG, "short write to disk (out of space?)");
        return ESP_FAIL;
    }
    sink->written += evt->data_len;
    return ESP_OK;
}

/* One url->dest pair. Returns true on success; on failure the partial file
 * (if any bytes made it to disk) is removed rather than left half-written. */
static bool fetch_one(const char *url, const char *dest)
{
    char dir_buf[192];
    snprintf(dir_buf, sizeof(dir_buf), "%s", dest);
    mkdir_p_parent(dir_buf);

    sink_t sink = { .file = fopen(dest, "wb"), .written = 0 };
    if (!sink.file) {
        ESP_LOGE(TAG, "cannot open '%s' for writing", dest);
        return false;
    }

    const esp_http_client_config_t cfg = {
        .url               = url,
        .method            = HTTP_METHOD_GET,
        .event_handler     = http_event,
        .user_data         = &sink,
        .timeout_ms        = 10000,
        .buffer_size       = 2048,
        .disable_auto_redirect = true,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        fclose(sink.file);
        remove(dest);
        return false;
    }

    const esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    fclose(sink.file);

    if (err != ESP_OK || status != 200) {
        ESP_LOGE(TAG, "GET '%s' failed: %s (HTTP %d)", url, esp_err_to_name(err), status);
        remove(dest);
        return false;
    }

    ESP_LOGI(TAG, "wrote %s (%u bytes)", dest, (unsigned)sink.written);
    return true;
}

/* {"items":[...]} or {"manifest_url":"..."} on the card — same convention
 * as /sd/wifi.json (#8) and /sd/ota.json (#17): a file, not something typed
 * on a keyboard this device does not have. */
static cJSON *load_local_manifest(void)
{
    char path[64];
    snprintf(path, sizeof(path), "%s/fetch.json", BSP_SD_MOUNT_POINT);

    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    char *buf = malloc(4096);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    const size_t n = fread(buf, 1, 4095, f);
    fclose(f);
    buf[n] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    return root;
}

typedef struct {
    char  *buf;
    size_t len;
} body_t;

static esp_err_t body_event(esp_http_client_event_t *evt)
{
    body_t *body = evt->user_data;
    if (evt->event_id != HTTP_EVENT_ON_DATA || !body) {
        return ESP_OK;
    }
    if (body->len + evt->data_len > FETCH_MANIFEST_MAX_BODY) {
        return ESP_FAIL;
    }
    memcpy(body->buf + body->len, evt->data, evt->data_len);
    body->len += evt->data_len;
    return ESP_OK;
}

/* The whole point of "manifest_url" (see fetch.h): the item list itself —
 * the thing that actually changes every time a new batch of files needs
 * pushing — lives on the PC, not the card. /sd/fetch.json becomes a
 * one-time bootstrap pointer, same spirit as /sd/agent.json holding just a
 * token rather than the whole protocol. Public (fetch_json_url() in
 * fetch.h) since catalog.c (ROADMAP #20) needs the exact same primitive for
 * both the catalog list itself and each app's own file list. */
cJSON *fetch_json_url(const char *url)
{
    body_t body = { .buf = malloc(FETCH_MANIFEST_MAX_BODY + 1), .len = 0 };
    if (!body.buf) {
        return NULL;
    }

    const esp_http_client_config_t cfg = {
        .url               = url,
        .method            = HTTP_METHOD_GET,
        .event_handler     = body_event,
        .user_data         = &body,
        .timeout_ms        = 10000,
        .buffer_size       = 2048,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        free(body.buf);
        return NULL;
    }

    const esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200) {
        ESP_LOGE(TAG, "manifest GET '%s' failed: %s (HTTP %d)", url, esp_err_to_name(err), status);
        free(body.buf);
        return NULL;
    }

    body.buf[body.len] = '\0';
    cJSON *root = cJSON_Parse(body.buf);
    free(body.buf);
    return root;
}

/* Resolves the local file to the manifest actually worth iterating: either
 * it already has "items" (the simple, everything-on-the-card case), or it
 * points at "manifest_url" and this fetches that instead. */
static cJSON *load_manifest(void)
{
    cJSON *root = load_local_manifest();
    if (!root) {
        return NULL;
    }
    const char *manifest_url = cJSON_GetStringValue(cJSON_GetObjectItem(root, "manifest_url"));
    if (manifest_url && *manifest_url) {
        cJSON *remote = fetch_json_url(manifest_url);
        cJSON_Delete(root);
        return remote;
    }
    return root;
}

/* The actual per-item download loop, shared between fetch_run() (fed from
 * /sd/fetch.json, possibly via manifest_url) and catalog.c's install path
 * (fed from a catalog entry's own manifest_url, fetched with
 * fetch_json_url() above) — same items shape, same reporting either way. */
bool fetch_items(const cJSON *items, fetch_progress_cb_t cb)
{
    const int total = cJSON_GetArraySize(items);
    int done_count = 0;
    int failed = 0;
    const cJSON *item;
    cJSON_ArrayForEach(item, items) {
        const char *url = cJSON_GetStringValue(cJSON_GetObjectItem(item, "url"));
        const char *dest = cJSON_GetStringValue(cJSON_GetObjectItem(item, "dest"));
        if (!url || !dest) {
            failed++;
            continue;
        }
        if (cb) {
            cb(FETCH_CONNECTING, (int)(((int64_t)done_count * 100) / total), dest);
        }
        if (!fetch_one(url, dest)) {
            failed++;
        }
        done_count++;
        if (cb) {
            cb(FETCH_DOWNLOADING, (int)(((int64_t)done_count * 100) / total), dest);
        }
    }
    return failed == 0;
}

static void fetch_task(void *arg)
{
    const fetch_progress_cb_t cb = (fetch_progress_cb_t)arg;

    cJSON *root = load_manifest();
    const cJSON *items = root ? cJSON_GetObjectItem(root, "items") : NULL;
    if (!cJSON_IsArray(items)) {
        ESP_LOGW(TAG, "no /sd/fetch.json (or no 'items' array) — nothing to fetch");
        if (cb) {
            cb(FETCH_ERROR, 0, "no /sd/fetch.json");
        }
        cJSON_Delete(root);
        goto done;
    }

    {
        const int total = cJSON_GetArraySize(items);
        const bool ok = fetch_items(items, cb);
        cJSON_Delete(root);

        if (cb) {
            if (!ok) {
                char detail[32];
                snprintf(detail, sizeof(detail), "some of %d failed", total);
                cb(FETCH_ERROR, 100, detail);
            } else {
                cb(FETCH_DONE, 100, NULL);
            }
        }
    }

done:
    s_running = false;
    vTaskDelete(NULL);
}

void fetch_run(fetch_progress_cb_t cb)
{
    if (s_running) {
        return;
    }
    s_running = true;
    /* Stack sized like ota's own download task (8192) — same shape of work
     * (esp_http_client + a JSON parse), just to a plain file instead of a
     * partition. */
    if (xTaskCreate(fetch_task, "fetch", 8192, (void *)cb, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "cannot start fetch task");
        s_running = false;
    }
}

bool fetch_is_running(void)
{
    return s_running;
}
