#include "webcfg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp_pins.h"
#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "net.h"

static const char *TAG = "webcfg";

#define DEVICE_PATH BSP_SD_MOUNT_POINT "/device.json"
#define WIFI_PATH   BSP_SD_MOUNT_POINT "/wifi.json"
#define AGENT_PATH  BSP_SD_MOUNT_POINT "/agent.json"
#define OTA_PATH    BSP_SD_MOUNT_POINT "/ota.json"
#define FETCH_PATH  BSP_SD_MOUNT_POINT "/fetch.json"
#define WEATHER_PATH BSP_SD_MOUNT_POINT "/weather.json"
#define CATALOG_PATH BSP_SD_MOUNT_POINT "/catalog.json"

static httpd_handle_t s_httpd;

/* Applying new WiFi credentials tears down and re-establishes the STA
 * connection this very request arrived over — doing that synchronously
 * inside the request handler, before the response goes out, raced the
 * reply on its way to the browser and killed it mid-transfer (found on
 * hardware: the page just hung and then dropped). net.c's own SoftAP setup
 * form hit the exact same race for the exact same reason and already
 * fixed it the same way: send the response first, apply after a short
 * delay once the bytes have actually had time to leave. */
static esp_timer_handle_t s_wifi_apply_timer;
static char s_pending_ssid[33];
static char s_pending_pass[65];

static void wifi_apply_cb(void *arg)
{
    (void)arg;
    net_set_credentials(s_pending_ssid, s_pending_pass);
}

/* -------------------------------------------------------------- JSON I/O */

/* Every field this form writes goes through here: read the file (if any),
 * set just this one field, write the whole thing back. An empty value
 * means "left blank on the form" and is treated as "don't touch this file
 * at all" — filling in only the WiFi section must not zero out an
 * already-working ota.json, and vice versa. Returns false without touching
 * anything on a blank value; that is the normal, expected case for every
 * field the user did not mean to change. */
static bool write_string_field(const char *path, const char *field, const char *value)
{
    if (!value || !*value) {
        return false;
    }

    cJSON *root = NULL;
    FILE *f = fopen(path, "rb");
    if (f) {
        /* Heap, not a stack local — esp_http_server's own worker task stack
         * is tight enough by default that a handful of these across the
         * handlers in this file overflowed it on real hardware (FreeRTOS
         * caught it: "A stack overflow in task httpd has been detected").
         * Same lesson as ROADMAP #11's widget.c cache buffer, different
         * task this time. */
        char *buf = malloc(512);
        if (buf) {
            const size_t n = fread(buf, 1, 511, f);
            buf[n] = '\0';
            root = cJSON_Parse(buf);
            free(buf);
        }
        fclose(f);
    }
    if (!root) {
        root = cJSON_CreateObject();
    }

    cJSON_DeleteItemFromObject(root, field);
    cJSON_AddStringToObject(root, field, value);

    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!text) {
        return false;
    }

    bool ok = false;
    FILE *wf = fopen(path, "wb");
    if (wf) {
        ok = fwrite(text, 1, strlen(text), wf) == strlen(text);
        fclose(wf);
    }
    cJSON_free(text);
    if (ok) {
        ESP_LOGI(TAG, "%s: updated '%s'", path, field);
    }
    return ok;
}

/* For prefilling the form — never called for a password/token field (see
 * build_page()'s own comment on why those stay blank). The read buffer is
 * heap, not stack, same as write_string_field() above and for the same
 * reason (see this file's comment on get_handler()'s own buffers). */
static bool read_string_field(const char *path, const char *field, char *out, size_t out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    char *buf = malloc(512);
    if (!buf) {
        fclose(f);
        return false;
    }
    const size_t n = fread(buf, 1, 511, f);
    fclose(f);
    buf[n] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        return false;
    }
    const char *v = cJSON_GetStringValue(cJSON_GetObjectItem(root, field));
    bool ok = false;
    if (v && *v) {
        snprintf(out, out_size, "%s", v);
        ok = true;
    }
    cJSON_Delete(root);
    return ok;
}

/* -------------------------------------------------------------- HTML glue */

/* Same two small helpers as net.c's SoftAP form (net_softap_start()) — not
 * shared with it because neither is exported from there, and each is a
 * handful of lines not worth a new shared component just for this. */
static void url_decode(char *dst, size_t dst_size, const char *src)
{
    size_t o = 0;
    for (size_t i = 0; src[i] && o + 1 < dst_size; i++) {
        char c = src[i];
        if (c == '+') {
            c = ' ';
        } else if (c == '%' && src[i + 1] && src[i + 2]) {
            const char hex[3] = { src[i + 1], src[i + 2], '\0' };
            c = (char)strtol(hex, NULL, 16);
            i += 2;
        }
        dst[o++] = c;
    }
    dst[o] = '\0';
}

static void html_escape(char *dst, size_t dst_size, const char *src)
{
    size_t o = 0;
    for (size_t i = 0; src[i] && o + 6 < dst_size; i++) {
        switch (src[i]) {
        case '<':  o += snprintf(dst + o, dst_size - o, "&lt;");   break;
        case '>':  o += snprintf(dst + o, dst_size - o, "&gt;");   break;
        case '&':  o += snprintf(dst + o, dst_size - o, "&amp;");  break;
        case '"':  o += snprintf(dst + o, dst_size - o, "&quot;"); break;
        default:   dst[o++] = src[i]; dst[o] = '\0';               break;
        }
    }
}

/* Every one of these used to be its own stack local in get_handler() — on
 * real hardware that overflowed esp_http_server's worker task stack (a
 * FreeRTOS-caught "stack overflow in task httpd", not a guess). One heap
 * allocation instead, same fix ROADMAP #11 already established for a
 * several-KB local on the LVGL task's own tight stack — the task is
 * different, the lesson is identical: a buffer this size does not belong
 * on a stack it does not own the budget of. */
typedef struct {
    char device_name[64], wifi_ssid[33], ota_url[192], ota_manifest[192], fetch_manifest[192];
    char weather_name[64], weather_lat[16], weather_lon[16], catalog_url[192];
    char safe_name[3 * 64], safe_ssid[3 * 33], safe_ota_url[3 * 192],
         safe_ota_manifest[3 * 192], safe_fetch_manifest[3 * 192];
    char safe_weather_name[3 * 64], safe_weather_lat[3 * 16], safe_weather_lon[3 * 16];
    char safe_catalog_url[3 * 192];
} form_values_t;

static esp_err_t get_handler(httpd_req_t *req)
{
    /* Non-secret fields are prefilled from whatever is on the card right
     * now, so this doubles as "what is currently configured", not just a
     * blank form. Password/token are NEVER prefilled — echoing a secret
     * back into a page is the one thing this form must not do, same rule
     * CLAUDE.md already states for logs. A blank password/token field on
     * submit leaves the existing one alone (see write_string_field()), so
     * there is no way to see the current value through this UI at all,
     * by design. */
    form_values_t *v = calloc(1, sizeof(form_values_t));
    char *page = malloc(5632);   /* bumped from 4608 for the App catalog fieldset — gcc's
                                   * format-truncation check does the arithmetic on the
                                   * worst case (every field maxed out, HTML-escaped to 3x)
                                   * and 4864 was already close enough to trip it */
    if (!v || !page) {
        free(v);
        free(page);
        return httpd_resp_send_500(req);
    }

    read_string_field(DEVICE_PATH, "name", v->device_name, sizeof(v->device_name));
    read_string_field(WIFI_PATH, "ssid", v->wifi_ssid, sizeof(v->wifi_ssid));
    read_string_field(OTA_PATH, "url", v->ota_url, sizeof(v->ota_url));
    read_string_field(OTA_PATH, "manifest_url", v->ota_manifest, sizeof(v->ota_manifest));
    read_string_field(FETCH_PATH, "manifest_url", v->fetch_manifest, sizeof(v->fetch_manifest));
    read_string_field(WEATHER_PATH, "name", v->weather_name, sizeof(v->weather_name));
    read_string_field(WEATHER_PATH, "lat", v->weather_lat, sizeof(v->weather_lat));
    read_string_field(WEATHER_PATH, "lon", v->weather_lon, sizeof(v->weather_lon));
    read_string_field(CATALOG_PATH, "catalog_url", v->catalog_url, sizeof(v->catalog_url));

    html_escape(v->safe_name, sizeof(v->safe_name), v->device_name);
    html_escape(v->safe_ssid, sizeof(v->safe_ssid), v->wifi_ssid);
    html_escape(v->safe_ota_url, sizeof(v->safe_ota_url), v->ota_url);
    html_escape(v->safe_ota_manifest, sizeof(v->safe_ota_manifest), v->ota_manifest);
    html_escape(v->safe_fetch_manifest, sizeof(v->safe_fetch_manifest), v->fetch_manifest);
    html_escape(v->safe_weather_name, sizeof(v->safe_weather_name), v->weather_name);
    html_escape(v->safe_weather_lat, sizeof(v->safe_weather_lat), v->weather_lat);
    html_escape(v->safe_weather_lon, sizeof(v->safe_weather_lon), v->weather_lon);
    html_escape(v->safe_catalog_url, sizeof(v->safe_catalog_url), v->catalog_url);

    snprintf(page, 5632,
        "<!doctype html><html><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>deskos config</title>"
        "<style>"
        "body{font-family:sans-serif;max-width:420px;margin:24px auto;padding:0 16px}"
        "fieldset{margin-bottom:16px;border:1px solid #ccc;border-radius:6px}"
        "label{display:block;margin:8px 0 4px}"
        "input{width:100%%;padding:8px;box-sizing:border-box}"
        "button{padding:10px 20px;margin-top:16px}"
        "p.hint{color:#777;font-size:0.85em;margin:2px 0 0}"
        "</style></head><body>"
        "<h2>deskos config</h2>"
        "<p class=\"hint\">Leave a field blank to leave it exactly as it is now — nothing "
        "gets cleared just because you did not retype it.</p>"
        "<form method=\"POST\" action=\"/save\">"
        "<fieldset><legend>Device</legend>"
        "<label>Name<input name=\"device_name\" maxlength=\"63\" value=\"%s\"></label>"
        "</fieldset>"
        "<fieldset><legend>WiFi</legend>"
        "<label>SSID<input name=\"wifi_ssid\" maxlength=\"32\" value=\"%s\"></label>"
        "<label>Password<input name=\"wifi_password\" type=\"password\" maxlength=\"64\"></label>"
        "</fieldset>"
        "<fieldset><legend>PC agent</legend>"
        "<label>Pairing token<input name=\"agent_token\" maxlength=\"64\"></label>"
        "<p class=\"hint\">From the agent's tray menu: Copy pairing token.</p>"
        "</fieldset>"
        "<fieldset><legend>Firmware update</legend>"
        "<label>Firmware URL (.bin)<input name=\"ota_url\" maxlength=\"191\" value=\"%s\"></label>"
        "<label>Update manifest URL (version/size JSON)"
        "<input name=\"ota_manifest_url\" maxlength=\"191\" value=\"%s\"></label>"
        "</fieldset>"
        "<fieldset><legend>Asset fetch</legend>"
        "<label>Manifest URL (icons, etc.)"
        "<input name=\"fetch_manifest_url\" maxlength=\"191\" value=\"%s\"></label>"
        "</fieldset>"
        "<fieldset><legend>Weather</legend>"
        "<label>City<input name=\"weather_name\" maxlength=\"63\" value=\"%s\"></label>"
        "<label>Latitude<input name=\"weather_lat\" maxlength=\"15\" value=\"%s\"></label>"
        "<label>Longitude<input name=\"weather_lon\" maxlength=\"15\" value=\"%s\"></label>"
        "<p class=\"hint\">Look coordinates up on openstreetmap.org (right-click a "
        "spot, \"Show address\") or latlong.net.</p>"
        "</fieldset>"
        "<fieldset><legend>App catalog</legend>"
        "<label>Catalog URL"
        "<input name=\"catalog_url\" maxlength=\"191\" value=\"%s\"></label>"
        "<p class=\"hint\">Settings &rarr; Apps &rarr; Browse catalog fetches this.</p>"
        "</fieldset>"
        "<button type=\"submit\">Save</button>"
        "</form></body></html>",
        v->safe_name, v->safe_ssid, v->safe_ota_url, v->safe_ota_manifest, v->safe_fetch_manifest,
        v->safe_weather_name, v->safe_weather_lat, v->safe_weather_lon, v->safe_catalog_url);

    httpd_resp_set_type(req, "text/html");
    const esp_err_t err = httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
    free(v);
    free(page);
    return err;
}

static esp_err_t post_handler(httpd_req_t *req)
{
    char *body = malloc(1024);
    if (!body) {
        return httpd_resp_send_500(req);
    }
    size_t want = (size_t)req->content_len;
    if (want > 1023) {
        want = 1023;
    }
    const int n = httpd_req_recv(req, body, want);
    if (n <= 0) {
        free(body);
        return httpd_resp_send_500(req);
    }
    body[n] = '\0';

    /* Heap, not stack locals — see get_handler()'s own comment on why this
     * task's stack does not have room for buffers this size. One raw+decoded
     * pair reused across every field: they are processed one at a time,
     * nothing needs to stay alive past its own write_string_field() call. */
    char *raw = malloc(192);
    char *decoded = malloc(192);
    if (!raw || !decoded) {
        free(body);
        free(raw);
        free(decoded);
        return httpd_resp_send_500(req);
    }
    int changed = 0;
    bool wifi_touched = false;

    #define FIELD(param, path, key)                                            \
        do {                                                                   \
            raw[0] = '\0';                                                     \
            httpd_query_key_value(body, param, raw, 192);                      \
            url_decode(decoded, 192, raw);                                     \
            if (write_string_field(path, key, decoded)) {                      \
                changed++;                                                     \
            }                                                                  \
        } while (0)

    FIELD("device_name",        DEVICE_PATH, "name");

    /* WiFi is a pair used together for one connection attempt, not two
     * independent fields — after writing whichever of the two the user
     * actually submitted (still through the same blank-means-leave-alone
     * primitive as everything else), read the *whole* pair back out of
     * wifi.json (so a blank password field does not get applied as "no
     * password" when only the SSID was being fixed) and hand it to
     * net_set_credentials(), the same call the SoftAP setup form uses, so
     * the change takes effect now instead of waiting for a manual reboot.
     * Plain write_string_field() alone would leave wifi.json — the
     * highest-priority credential source, see CLAUDE.md — updated but
     * silently unapplied until the next boot. */
    raw[0] = '\0';
    httpd_query_key_value(body, "wifi_ssid", raw, 192);
    url_decode(decoded, 192, raw);
    if (write_string_field(WIFI_PATH, "ssid", decoded)) {
        wifi_touched = true;
        changed++;
    }
    raw[0] = '\0';
    httpd_query_key_value(body, "wifi_password", raw, 192);
    url_decode(decoded, 192, raw);
    if (write_string_field(WIFI_PATH, "password", decoded)) {
        wifi_touched = true;
        changed++;
    }
    if (wifi_touched) {
        /* Into the static pending buffers, not applied yet — see
         * wifi_apply_cb()'s own comment on why this waits until after the
         * response for this very request has gone out. */
        s_pending_ssid[0] = s_pending_pass[0] = '\0';
        read_string_field(WIFI_PATH, "ssid", s_pending_ssid, sizeof(s_pending_ssid));
        read_string_field(WIFI_PATH, "password", s_pending_pass, sizeof(s_pending_pass));
    }

    FIELD("agent_token",        AGENT_PATH,  "token");
    FIELD("ota_url",            OTA_PATH,    "url");
    FIELD("ota_manifest_url",   OTA_PATH,    "manifest_url");
    FIELD("fetch_manifest_url", FETCH_PATH,  "manifest_url");
    FIELD("weather_name",       WEATHER_PATH, "name");
    FIELD("weather_lat",        WEATHER_PATH, "lat");
    FIELD("weather_lon",        WEATHER_PATH, "lon");
    FIELD("catalog_url",        CATALOG_PATH, "catalog_url");

    #undef FIELD

    free(body);
    free(raw);
    free(decoded);

    char page[384];
    snprintf(page, sizeof(page),
        "<!doctype html><html><body style=\"font-family:sans-serif;max-width:420px;"
        "margin:24px auto;padding:0 16px\"><h2>Saved</h2>"
        "<p>%d field(s) updated. Anything left blank was not touched.</p>"
        "%s"
        "<p><a href=\"/\">Back</a></p></body></html>",
        changed,
        wifi_touched ? "<p>Reconnecting to the new WiFi network shortly — if it "
                       "is different from this one, this page will stop "
                       "responding once that happens.</p>" : "");
    httpd_resp_set_type(req, "text/html");
    const esp_err_t err = httpd_resp_sendstr(req, page);

    if (wifi_touched) {
        esp_timer_start_once(s_wifi_apply_timer, 800 * 1000);
    }
    return err;
}

esp_err_t webcfg_start(void)
{
    if (s_httpd) {
        return ESP_OK;
    }

    char ip[16];
    if (!net_get_ip(ip, sizeof(ip))) {
        ESP_LOGW(TAG, "cannot start — WiFi is not up");
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_wifi_apply_timer) {
        const esp_timer_create_args_t targs = {
            .callback = wifi_apply_cb,
            .name = "webcfg_wifi_apply",
        };
        if (esp_timer_create(&targs, &s_wifi_apply_timer) != ESP_OK) {
            ESP_LOGE(TAG, "cannot create WiFi-apply timer");
            return ESP_FAIL;
        }
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = WEBCFG_PORT;
    cfg.lru_purge_enable = true;
    /* Default (4096) is exactly what a real stack overflow on hardware was
     * traced to (see get_handler()'s own comment) — moving this file's own
     * buffers to the heap fixed the actual cause, this is the belt-and-
     * suspenders margin on top of that for whatever baseline the HTTP
     * parsing/dispatch machinery itself needs underneath a handler. */
    cfg.stack_size = 8192;

    const esp_err_t err = httpd_start(&s_httpd, &cfg);
    if (err != ESP_OK) {
        s_httpd = NULL;
        return err;
    }

    static const httpd_uri_t get_uri  = { .uri = "/",     .method = HTTP_GET,  .handler = get_handler };
    static const httpd_uri_t post_uri = { .uri = "/save", .method = HTTP_POST, .handler = post_handler };
    httpd_register_uri_handler(s_httpd, &get_uri);
    httpd_register_uri_handler(s_httpd, &post_uri);

    ESP_LOGI(TAG, "started at http://%s:%d", ip, WEBCFG_PORT);
    return ESP_OK;
}

void webcfg_stop(void)
{
    if (!s_httpd) {
        return;
    }
    httpd_stop(s_httpd);
    s_httpd = NULL;
}

bool webcfg_is_running(void)
{
    return s_httpd != NULL;
}
