#include "datasource.h"

#include <stdio.h>
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

const cJSON *datasource_resolve(const cJSON *root, const char *path)
{
    if (!root || !path || !*path) {
        return NULL;
    }

    const cJSON *node = root;
    const char *p = path;

    while (*p && node) {
        char key[48];
        size_t n = 0;
        while (*p && *p != '.' && n < sizeof(key) - 1) {
            key[n++] = *p++;
        }
        key[n] = '\0';
        if (*p == '.') {
            p++;
        }
        if (n == 0) {
            return NULL;
        }

        /* An all-digits component indexes an array; anything else is a key. */
        bool numeric = true;
        for (size_t i = 0; i < n; i++) {
            if (key[i] < '0' || key[i] > '9') {
                numeric = false;
                break;
            }
        }

        if (numeric && cJSON_IsArray(node)) {
            node = cJSON_GetArrayItem(node, atoi(key));
        } else {
            node = cJSON_GetObjectItemCaseSensitive(node, key);
        }
    }

    return node;
}

static void append_value(const cJSON *node, char *out, size_t out_size, size_t *used)
{
    char tmp[48];

    if (!node) {
        snprintf(tmp, sizeof(tmp), "--");
    } else if (cJSON_IsString(node)) {
        snprintf(tmp, sizeof(tmp), "%s", node->valuestring);
    } else if (cJSON_IsNumber(node)) {
        const double v = node->valuedouble;
        /* Integers should not render as "3.0"; everything else keeps one
         * decimal, which is the useful precision for temperatures and rates. */
        if (v == (double)(long long)v) {
            snprintf(tmp, sizeof(tmp), "%lld", (long long)v);
        } else {
            snprintf(tmp, sizeof(tmp), "%.1f", v);
        }
    } else if (cJSON_IsBool(node)) {
        snprintf(tmp, sizeof(tmp), "%s", cJSON_IsTrue(node) ? "yes" : "no");
    } else {
        snprintf(tmp, sizeof(tmp), "--");
    }

    for (const char *s = tmp; *s && *used < out_size - 1; s++) {
        out[(*used)++] = *s;
    }
}

void datasource_render(const char *tmpl, const cJSON *root, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }
    if (!tmpl) {
        out[0] = '\0';
        return;
    }

    size_t used = 0;
    const char *p = tmpl;

    while (*p && used < out_size - 1) {
        if (p[0] == '{' && p[1] == '{') {
            const char *end = strstr(p + 2, "}}");
            if (end) {
                char path[64];
                const size_t n = (size_t)(end - (p + 2));
                if (n < sizeof(path)) {
                    memcpy(path, p + 2, n);
                    path[n] = '\0';
                    append_value(datasource_resolve(root, path), out, out_size, &used);
                }
                p = end + 2;
                continue;
            }
        }
        out[used++] = *p++;
    }

    out[used] = '\0';
}
