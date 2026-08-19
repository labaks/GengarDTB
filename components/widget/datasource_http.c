/*
 * The one impure part of the datasource module: a blocking HTTPS GET. Kept
 * apart from datasource.c so the pure resolve/render logic can be built and
 * tested with a plain host compiler, with no ESP-IDF network stack in sight.
 */
#include "datasource.h"

#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"

static const char *TAG = "datasource";

typedef struct {
    char  *buf;
    size_t len;
} body_t;

static esp_err_t http_event(esp_http_client_event_t *evt)
{
    body_t *body = evt->user_data;

    if (evt->event_id != HTTP_EVENT_ON_DATA || !body) {
        return ESP_OK;
    }
    if (body->len + evt->data_len > DATASOURCE_MAX_BODY) {
        ESP_LOGW(TAG, "response exceeds %d bytes, truncating", DATASOURCE_MAX_BODY);
        return ESP_FAIL;
    }

    memcpy(body->buf + body->len, evt->data, evt->data_len);
    body->len += evt->data_len;
    return ESP_OK;
}

esp_err_t datasource_fetch_json(const char *url, cJSON **out_json)
{
    if (!url || !*url || !out_json) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_json = NULL;

    body_t body = { .buf = malloc(DATASOURCE_MAX_BODY + 1), .len = 0 };
    if (!body.buf) {
        return ESP_ERR_NO_MEM;
    }

    const esp_http_client_config_t cfg = {
        .url               = url,
        .method            = HTTP_METHOD_GET,
        .event_handler     = http_event,
        .user_data         = &body,
        .timeout_ms        = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size       = 1024,
        /* Layer-A sources are public read-only endpoints; a redirect to some
         * other host is not something we follow silently. */
        .disable_auto_redirect = true,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        free(body.buf);
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "GET failed: %s", esp_err_to_name(err));
        free(body.buf);
        return err;
    }
    if (status != 200) {
        ESP_LOGW(TAG, "GET returned HTTP %d", status);
        free(body.buf);
        return ESP_ERR_INVALID_RESPONSE;
    }

    body.buf[body.len] = '\0';
    cJSON *root = cJSON_Parse(body.buf);
    free(body.buf);

    if (!root) {
        ESP_LOGW(TAG, "response is not valid JSON");
        return ESP_ERR_INVALID_RESPONSE;
    }

    *out_json = root;
    return ESP_OK;
}
