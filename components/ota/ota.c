#include "ota.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp_pins.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ota";

#define OTA_MANIFEST_MAX_BODY 1024

static volatile bool s_running;

/* {"url":"http://host/deskos.bin","manifest_url":"http://host/update.json"}
 * on the microSD card — same convention as /sd/wifi.json (#8) and
 * /sd/agent.json (#12/#13): a file, not something typed on a keyboard this
 * device does not have. "manifest_url" is only read by ota_check(); older
 * files without it simply can't be checked (ota_start_update() itself only
 * ever needed "url"). */
static bool load_json_field(const char *field, char *out, size_t out_size)
{
    char path[64];
    snprintf(path, sizeof(path), "%s/ota.json", BSP_SD_MOUNT_POINT);

    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    char buf[256];
    const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    cJSON *root = cJSON_Parse(buf);
    bool ok = false;
    if (root) {
        const char *value = cJSON_GetStringValue(cJSON_GetObjectItem(root, field));
        if (value && *value) {
            snprintf(out, out_size, "%s", value);
            ok = true;
        }
        cJSON_Delete(root);
    }
    return ok;
}

/* Plain X.Y.Z compare — this project's own version.txt (ROADMAP #38.3) never
 * carries a pre-release/build suffix, so none is parsed here. Missing
 * components count as 0 ("1.2" == "1.2.0"). Returns <0/0/>0 like strcmp. */
static int semver_cmp(const char *a, const char *b)
{
    int an, bn;
    for (int i = 0; i < 3; i++) {
        an = atoi(a);
        bn = atoi(b);
        if (an != bn) {
            return an - bn;
        }
        a = strchr(a, '.');
        b = strchr(b, '.');
        a = a ? a + 1 : "0";
        b = b ? b + 1 : "0";
    }
    return 0;
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
    if (body->len + evt->data_len > OTA_MANIFEST_MAX_BODY) {
        return ESP_FAIL;
    }
    memcpy(body->buf + body->len, evt->data, evt->data_len);
    body->len += evt->data_len;
    return ESP_OK;
}

/* Deliberately never touches esp_https_ota/esp_ota_ops — see ota.h's own
 * comment on why the two entry points are split. Just a plain GET of a
 * small JSON document. */
static void ota_check_task(void *arg)
{
    const ota_check_cb_t cb = (ota_check_cb_t)arg;
    char manifest_url[256];

    if (cb) {
        cb(OTA_CHECK_CHECKING, NULL, 0, NULL);
    }

    if (!load_json_field("manifest_url", manifest_url, sizeof(manifest_url))) {
        ESP_LOGW(TAG, "no manifest_url in /sd/ota.json — nothing to check against");
        if (cb) {
            cb(OTA_CHECK_ERROR, NULL, 0, "no manifest_url in /sd/ota.json");
        }
        goto done;
    }

    {
        body_t body = { .buf = malloc(OTA_MANIFEST_MAX_BODY + 1), .len = 0 };
        if (!body.buf) {
            if (cb) {
                cb(OTA_CHECK_ERROR, NULL, 0, "out of memory");
            }
            goto done;
        }

        const esp_http_client_config_t cfg = {
            .url               = manifest_url,
            .method            = HTTP_METHOD_GET,
            .event_handler     = body_event,
            .user_data         = &body,
            .timeout_ms        = 10000,
            .buffer_size       = 1024,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .disable_auto_redirect = true,
        };
        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        if (!client) {
            free(body.buf);
            if (cb) {
                cb(OTA_CHECK_ERROR, NULL, 0, "cannot init HTTP client");
            }
            goto done;
        }

        const esp_err_t err = esp_http_client_perform(client);
        const int status = esp_http_client_get_status_code(client);
        esp_http_client_cleanup(client);

        if (err != ESP_OK || status != 200) {
            ESP_LOGW(TAG, "manifest GET '%s' failed: %s (HTTP %d)", manifest_url, esp_err_to_name(err), status);
            free(body.buf);
            if (cb) {
                cb(OTA_CHECK_ERROR, NULL, 0, "manifest fetch failed");
            }
            goto done;
        }

        body.buf[body.len] = '\0';
        cJSON *root = cJSON_Parse(body.buf);
        free(body.buf);

        if (!root) {
            if (cb) {
                cb(OTA_CHECK_ERROR, NULL, 0, "manifest is not valid JSON");
            }
            goto done;
        }

        const char *version = cJSON_GetStringValue(cJSON_GetObjectItem(root, "version"));
        const cJSON *size_item = cJSON_GetObjectItem(root, "size");
        const size_t size_bytes = cJSON_IsNumber(size_item) ? (size_t)cJSON_GetNumberValue(size_item) : 0;

        if (!version || !*version) {
            cJSON_Delete(root);
            if (cb) {
                cb(OTA_CHECK_ERROR, NULL, 0, "manifest has no 'version'");
            }
            goto done;
        }

        const char *current = esp_app_get_description()->version;
        const bool newer = semver_cmp(version, current) > 0;
        ESP_LOGI(TAG, "update check: running '%s', manifest '%s' (%s)",
                 current, version, newer ? "newer" : "not newer");

        char version_copy[32];
        snprintf(version_copy, sizeof(version_copy), "%s", version);
        cJSON_Delete(root);

        if (cb) {
            cb(newer ? OTA_CHECK_AVAILABLE : OTA_CHECK_UP_TO_DATE, version_copy, size_bytes, NULL);
        }
    }

done:
    s_running = false;
    vTaskDelete(NULL);
}

void ota_check(ota_check_cb_t cb)
{
    if (s_running) {
        return;
    }
    s_running = true;
    if (xTaskCreate(ota_check_task, "ota_check", 4096, (void *)cb, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "cannot start OTA check task");
        s_running = false;
    }
}

static void ota_update_task(void *arg)
{
    const ota_progress_cb_t cb = (ota_progress_cb_t)arg;
    char url[256];
    esp_https_ota_handle_t handle = NULL;
    esp_err_t err;

    if (!load_json_field("url", url, sizeof(url))) {
        ESP_LOGW(TAG, "no /sd/ota.json — nothing to update from");
        if (cb) {
            cb(OTA_ERROR, 0, "no /sd/ota.json");
        }
        goto done;
    }

    if (cb) {
        cb(OTA_CONNECTING, 0, NULL);
    }

    {
        const esp_http_client_config_t http_cfg = {
            .url = url,
            .timeout_ms = 15000,
            .keep_alive_enable = true,
            .crt_bundle_attach = esp_crt_bundle_attach,   /* only matters for https:// */
        };
        const esp_https_ota_config_t ota_cfg = { .http_config = &http_cfg };

        err = esp_https_ota_begin(&ota_cfg, &handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "cannot start OTA: %s", esp_err_to_name(err));
            if (cb) {
                cb(OTA_ERROR, 0, esp_err_to_name(err));
            }
            goto done;
        }
    }

    {
        esp_app_desc_t new_desc;
        if (esp_https_ota_get_img_desc(handle, &new_desc) == ESP_OK) {
            ESP_LOGI(TAG, "current '%s' -> new '%s'",
                     esp_app_get_description()->version, new_desc.version);
            if (cb) {
                cb(OTA_DOWNLOADING, 0, new_desc.version);
            }
        }
    }

    do {
        err = esp_https_ota_perform(handle);
        const int total = esp_https_ota_get_image_size(handle);
        const int read = esp_https_ota_get_image_len_read(handle);
        if (cb && total > 0) {
            cb(OTA_DOWNLOADING, (int)(((int64_t)read * 100) / total), NULL);
        }
    } while (err == ESP_ERR_HTTPS_OTA_IN_PROGRESS);

    if (err != ESP_OK || !esp_https_ota_is_complete_data_received(handle)) {
        ESP_LOGE(TAG, "OTA download failed: %s", esp_err_to_name(err));
        esp_https_ota_abort(handle);
        if (cb) {
            cb(OTA_ERROR, 0, esp_err_to_name(err));
        }
        goto done;
    }

    err = esp_https_ota_finish(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA finish failed: %s", esp_err_to_name(err));
        if (cb) {
            cb(OTA_ERROR, 0, esp_err_to_name(err));
        }
        goto done;
    }

    /* The new image is already the boot partition at this point — the
     * currently running (old) firmware just keeps going until the reboot
     * below actually switches over. */
    ESP_LOGI(TAG, "OTA complete, rebooting into the new image");
    if (cb) {
        cb(OTA_DONE, 100, NULL);
    }
    vTaskDelay(pdMS_TO_TICKS(2000));   /* let the UI show the "done" message before the reset */
    esp_restart();

done:
    s_running = false;
    vTaskDelete(NULL);
}

void ota_start_update(ota_progress_cb_t cb)
{
    if (s_running) {
        return;
    }
    s_running = true;
    if (xTaskCreate(ota_update_task, "ota", 8192, (void *)cb, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "cannot start OTA update task");
        s_running = false;
    }
}

bool ota_is_running(void)
{
    return s_running;
}
