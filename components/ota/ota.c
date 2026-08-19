#include "ota.h"

#include <stdio.h>
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

static volatile bool s_running;

/* {"url":"http://host/deskos.bin"} on the microSD card — same convention as
 * /sd/wifi.json (#8) and /sd/agent.json (#12/#13): a file, not something
 * typed on a keyboard this device does not have. */
static bool load_url(char *out, size_t out_size)
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
        const char *url = cJSON_GetStringValue(cJSON_GetObjectItem(root, "url"));
        if (url && *url) {
            snprintf(out, out_size, "%s", url);
            ok = true;
        }
        cJSON_Delete(root);
    }
    return ok;
}

static void ota_task(void *arg)
{
    const ota_progress_cb_t cb = (ota_progress_cb_t)arg;
    char url[256];
    esp_https_ota_handle_t handle = NULL;
    esp_err_t err;

    if (!load_url(url, sizeof(url))) {
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

void ota_check_and_update(ota_progress_cb_t cb)
{
    if (s_running) {
        return;
    }
    s_running = true;
    if (xTaskCreate(ota_task, "ota", 8192, (void *)cb, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "cannot start OTA task");
        s_running = false;
    }
}

bool ota_is_running(void)
{
    return s_running;
}
