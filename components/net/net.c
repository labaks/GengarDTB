#include "net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bsp_pins.h"
#include "cJSON.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "net";

#define NVS_NAMESPACE "deskos"
#define NVS_KEY_SSID  "wifi_ssid"
#define NVS_KEY_PASS  "wifi_pass"
#define NVS_KEY_TZ    "tz"

#define WIFI_JSON_PATH BSP_SD_MOUNT_POINT "/wifi.json"

/* Reconnect backoff. Capped rather than unbounded: this device sits on a desk
 * and the access point may simply be off for the night — it should keep trying
 * forever, just not every second. */
#define RETRY_MIN_MS  2000
#define RETRY_MAX_MS  60000

static net_state_t    s_state;
static net_state_cb_t s_cb;
static char           s_ssid[33];
static char           s_pass[65];
static uint32_t       s_retry_ms = RETRY_MIN_MS;
static bool           s_time_valid;
static esp_netif_t   *s_sta_netif;

static void set_state(net_state_t st)
{
    if (s_state == st) {
        return;
    }
    s_state = st;
    ESP_LOGI(TAG, "state -> %s", st == NET_UP ? "UP" : st == NET_CONNECTING ? "CONNECTING" : "DOWN");
    if (s_cb) {
        s_cb(st);
    }
}

/* wifi_config_t fields are fixed-size byte arrays that need not be
 * NUL-terminated when full, so they are filled by length rather than by string
 * copy — our buffers are one byte larger to hold a terminator, which is exactly
 * the mismatch the compiler flags. */
static void fill_sta_config(wifi_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    memcpy(cfg->sta.ssid, s_ssid, strnlen(s_ssid, sizeof(cfg->sta.ssid)));
    memcpy(cfg->sta.password, s_pass, strnlen(s_pass, sizeof(cfg->sta.password)));
}

/* ------------------------------------------------------------- credentials */

/* {"ssid":"...","password":"..."} on the microSD card. Swapping the card is
 * the easiest way to move the device to a different network or hand it to
 * someone else, without a rebuild and without touching NVS. When present, it
 * always wins over whatever NVS holds. */
static bool load_credentials_from_sd(void)
{
    FILE *f = fopen(WIFI_JSON_PATH, "rb");
    if (!f) {
        return false;
    }

    char buf[320];
    const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        ESP_LOGW(TAG, "%s: not valid JSON, ignoring", WIFI_JSON_PATH);
        return false;
    }

    const char *ssid = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "ssid"));
    bool ok = false;
    if (ssid && *ssid) {
        const char *pass = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "password"));
        snprintf(s_ssid, sizeof(s_ssid), "%s", ssid);
        snprintf(s_pass, sizeof(s_pass), "%s", pass ? pass : "");
        ok = true;
    } else {
        ESP_LOGW(TAG, "%s: missing 'ssid', ignoring", WIFI_JSON_PATH);
    }

    cJSON_Delete(root);
    if (ok) {
        /* Never log the password — only the SSID, same rule as everywhere else here. */
        ESP_LOGI(TAG, "credentials loaded from %s (ssid '%s')", WIFI_JSON_PATH, s_ssid);
    }
    return ok;
}

static bool load_credentials(void)
{
    s_ssid[0] = s_pass[0] = '\0';

    if (load_credentials_from_sd()) {
        return true;
    }

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        size_t n = sizeof(s_ssid);
        if (nvs_get_str(h, NVS_KEY_SSID, s_ssid, &n) != ESP_OK) {
            s_ssid[0] = '\0';
        }
        n = sizeof(s_pass);
        if (nvs_get_str(h, NVS_KEY_PASS, s_pass, &n) != ESP_OK) {
            s_pass[0] = '\0';
        }
        nvs_close(h);
    }

    if (s_ssid[0] == '\0') {
        /* Fall back to the build-time default. */
        snprintf(s_ssid, sizeof(s_ssid), "%s", CONFIG_DESKOS_WIFI_SSID);
        snprintf(s_pass, sizeof(s_pass), "%s", CONFIG_DESKOS_WIFI_PASSWORD);
    }

    return s_ssid[0] != '\0';
}

static esp_err_t persist_credentials(const char *ssid, const char *password)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h), TAG, "nvs open");

    esp_err_t err;
    if (!ssid || !*ssid) {
        nvs_erase_key(h, NVS_KEY_SSID);
        nvs_erase_key(h, NVS_KEY_PASS);
        err = ESP_OK;
    } else {
        err = nvs_set_str(h, NVS_KEY_SSID, ssid);
        if (err == ESP_OK) {
            err = nvs_set_str(h, NVS_KEY_PASS, password ? password : "");
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t net_set_credentials(const char *ssid, const char *password)
{
    const esp_err_t err = persist_credentials(ssid, password);
    if (err != ESP_OK) {
        return err;
    }

    /* Never log the password, and never log the SSID at anything but INFO. */
    ESP_LOGI(TAG, "credentials updated (ssid '%s')", ssid ? ssid : "<cleared>");

    esp_wifi_disconnect();

    bool have_creds;
    if (ssid && *ssid) {
        /* Apply what was just passed in directly, rather than re-deriving it
         * through load_credentials(). That function checks /sd/wifi.json
         * first by design (see CLAUDE.md) — correct on boot, but wrong here:
         * credentials someone just typed through the setup screen must win
         * immediately, not be silently overridden by a file still sitting on
         * the card from a previous owner or network. */
        snprintf(s_ssid, sizeof(s_ssid), "%s", ssid);
        snprintf(s_pass, sizeof(s_pass), "%s", password ? password : "");
        have_creds = true;
    } else {
        /* Cleared: fall through the normal priority chain (SD file, then the
         * build-time default) instead of going straight to "no network". */
        have_creds = load_credentials();
    }

    if (have_creds) {
        wifi_config_t cfg;
        fill_sta_config(&cfg);
        ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &cfg), TAG, "set config");
        s_retry_ms = RETRY_MIN_MS;
        esp_wifi_connect();
        set_state(NET_CONNECTING);
    } else {
        set_state(NET_DOWN);
    }
    return ESP_OK;
}

bool net_get_ssid(char *out, size_t out_size)
{
    if (!out || out_size == 0 || s_ssid[0] == '\0') {
        return false;
    }
    snprintf(out, out_size, "%s", s_ssid);
    return true;
}

bool net_get_ip(char *out, size_t out_size)
{
    if (!out || out_size == 0 || s_state != NET_UP || !s_sta_netif) {
        return false;
    }
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(s_sta_netif, &ip_info) != ESP_OK) {
        return false;
    }
    snprintf(out, out_size, IPSTR, IP2STR(&ip_info.ip));
    return true;
}

/* /sd/device.json ({"name":"..."}) — ROADMAP #38.2/#45, same file
 * settings.c's About screen reads. Small local duplicate rather than a
 * shared getter: this is the only other place that needs it, and it is a
 * handful of lines. */
void net_get_softap_ssid(char *out, size_t out_size)
{
    char path[64];
    snprintf(path, sizeof(path), "%s/device.json", BSP_SD_MOUNT_POINT);

    FILE *f = fopen(path, "rb");
    if (f) {
        char buf[128];
        const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[n] = '\0';
        cJSON *root = cJSON_Parse(buf);
        if (root) {
            const char *v = cJSON_GetStringValue(cJSON_GetObjectItem(root, "name"));
            if (v && *v) {
                snprintf(out, out_size, "%s", v);
                cJSON_Delete(root);
                return;
            }
            cJSON_Delete(root);
        }
    }
    snprintf(out, out_size, "%s", NET_SOFTAP_SSID);
}

/* --------------------------------------------------------- WiFi setup (AP) */

/* There is no keyboard on this device, so entering an arbitrary SSID/password
 * happens on a phone or laptop instead: the device opens its own open AP and
 * serves one plain HTML form at 192.168.4.1 (the address esp_netif hands out
 * to the AP interface by default — same address the factory firmware used
 * for its captive portal, see CLAUDE.md). No DNS hijack / auto-popup: the
 * setup screen just tells the user to open the address manually. */

static esp_netif_t      *s_ap_netif;
static httpd_handle_t    s_softap_httpd;
static esp_timer_handle_t s_softap_apply_timer;
static bool               s_softap_active;
static char               s_pending_ssid[sizeof(s_ssid)];
static char               s_pending_pass[sizeof(s_pass)];

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

/* Escapes the SSID before echoing it back into the confirmation page. Cheap
 * insurance: the device is on the user's own desk and the page is torn down
 * within a second, but there is no reason to trust bytes from an HTTP body. */
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

static esp_err_t softap_get_handler(httpd_req_t *req)
{
    static const char PAGE[] =
        "<!doctype html><html><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>deskos WiFi setup</title></head>"
        "<body style=\"font-family:sans-serif;max-width:360px;margin:40px auto;padding:0 16px\">"
        "<h2>deskos WiFi setup</h2>"
        "<form method=\"POST\" action=\"/save\">"
        "<label>Network name (SSID)<br>"
        "<input name=\"ssid\" maxlength=\"32\" required style=\"width:100%;padding:8px;margin:8px 0\">"
        "</label><br>"
        "<label>Password<br>"
        "<input name=\"password\" type=\"password\" maxlength=\"64\" style=\"width:100%;padding:8px;margin:8px 0\">"
        "</label><br>"
        "<button type=\"submit\" style=\"padding:10px 20px;margin-top:8px\">Connect</button>"
        "</form></body></html>";
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, PAGE, HTTPD_RESP_USE_STRLEN);
}

static void softap_apply_cb(void *arg)
{
    (void)arg;
    if (s_softap_httpd) {
        httpd_stop(s_softap_httpd);
        s_softap_httpd = NULL;
    }
    s_softap_active = false;

    const esp_err_t err = persist_credentials(s_pending_ssid, s_pending_pass);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to save credentials from setup: %s", esp_err_to_name(err));
    }
    snprintf(s_ssid, sizeof(s_ssid), "%s", s_pending_ssid);
    snprintf(s_pass, sizeof(s_pass), "%s", s_pending_pass);
    ESP_LOGI(TAG, "credentials updated (ssid '%s')", s_ssid);

    /* Mode switch then config, same order as net_init() at boot — NOT
     * followed by our own esp_wifi_connect(). Verified on hardware: calling
     * connect() here as well as relying on the WIFI_EVENT_STA_START handler
     * (which also calls it once s_ssid is set) raced the driver — logged
     * "wifi:sta is connecting, return error" and then never associated,
     * looping through backoff retries forever. One mode switch, one connect
     * call, and STA_START is the one that gets to make it. */
    esp_wifi_set_mode(WIFI_MODE_STA);
    wifi_config_t cfg;
    fill_sta_config(&cfg);
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    s_retry_ms = RETRY_MIN_MS;
}

static esp_err_t softap_post_handler(httpd_req_t *req)
{
    char body[256];
    size_t want = (size_t)req->content_len;
    if (want > sizeof(body) - 1) {
        want = sizeof(body) - 1;
    }
    const int n = httpd_req_recv(req, body, want);
    if (n <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    body[n] = '\0';

    char raw_ssid[96] = "", raw_pass[192] = "";
    httpd_query_key_value(body, "ssid", raw_ssid, sizeof(raw_ssid));
    httpd_query_key_value(body, "password", raw_pass, sizeof(raw_pass));
    url_decode(s_pending_ssid, sizeof(s_pending_ssid), raw_ssid);
    url_decode(s_pending_pass, sizeof(s_pending_pass), raw_pass);

    if (s_pending_ssid[0] == '\0') {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/html");
        httpd_resp_sendstr(req, "<p>SSID is required. <a href=\"/\">Back</a></p>");
        return ESP_OK;
    }

    char safe_ssid[3 * sizeof(s_pending_ssid)];
    html_escape(safe_ssid, sizeof(safe_ssid), s_pending_ssid);

    char page[384];
    snprintf(page, sizeof(page),
        "<!doctype html><html><body style=\"font-family:sans-serif;max-width:360px;"
        "margin:40px auto;padding:0 16px\"><h2>Saved</h2>"
        "<p>deskos is reconnecting to &quot;%s&quot; now. "
        "This page's WiFi will disappear shortly.</p></body></html>", safe_ssid);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, page);

    /* Apply after the response has had time to reach the phone — tearing the
     * AP down from inside this handler would race the reply on its way out. */
    esp_timer_start_once(s_softap_apply_timer, 800 * 1000);
    return ESP_OK;
}

esp_err_t net_softap_start(void)
{
    if (s_softap_active) {
        return ESP_OK;
    }

    if (!s_ap_netif) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        ESP_RETURN_ON_FALSE(s_ap_netif, ESP_FAIL, TAG, "ap netif");
    }
    if (!s_softap_apply_timer) {
        const esp_timer_create_args_t targs = {
            .callback = softap_apply_cb,
            .name = "wifi_setup_apply",
        };
        ESP_RETURN_ON_ERROR(esp_timer_create(&targs, &s_softap_apply_timer), TAG, "timer create");
    }

    s_softap_active = true;
    esp_wifi_disconnect();
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "ap mode");

    wifi_config_t ap_cfg;
    memset(&ap_cfg, 0, sizeof(ap_cfg));
    /* Sized to match ap_cfg.ap.ssid exactly (32 bytes + NUL) — a device name
     * longer than that (the web form allows up to 63) is truncated here,
     * same as any WiFi SSID would be regardless of where it came from. */
    char ssid[sizeof(ap_cfg.ap.ssid)];
    net_get_softap_ssid(ssid, sizeof(ssid));
    snprintf((char *)ap_cfg.ap.ssid, sizeof(ap_cfg.ap.ssid), "%s", ssid);
    ap_cfg.ap.ssid_len = (uint8_t)strlen((char *)ap_cfg.ap.ssid);
    ap_cfg.ap.channel = 1;
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;   /* short-lived, user-triggered — see CLAUDE.md */
    ap_cfg.ap.max_connection = 2;
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg), TAG, "ap config");

    httpd_config_t http_cfg = HTTPD_DEFAULT_CONFIG();
    http_cfg.lru_purge_enable = true;
    const esp_err_t err = httpd_start(&s_softap_httpd, &http_cfg);
    if (err != ESP_OK) {
        net_softap_stop();
        return err;
    }

    static const httpd_uri_t get_uri  = { .uri = "/",     .method = HTTP_GET,  .handler = softap_get_handler };
    static const httpd_uri_t post_uri = { .uri = "/save", .method = HTTP_POST, .handler = softap_post_handler };
    httpd_register_uri_handler(s_softap_httpd, &get_uri);
    httpd_register_uri_handler(s_softap_httpd, &post_uri);

    set_state(NET_SOFTAP);
    ESP_LOGI(TAG, "setup AP up: ssid '%s', %s", NET_SOFTAP_SSID, NET_SOFTAP_URL);
    return ESP_OK;
}

void net_softap_stop(void)
{
    if (!s_softap_active) {
        return;
    }
    s_softap_active = false;

    if (s_softap_apply_timer) {
        esp_timer_stop(s_softap_apply_timer);
    }
    if (s_softap_httpd) {
        httpd_stop(s_softap_httpd);
        s_softap_httpd = NULL;
    }

    const bool have_creds = load_credentials();
    esp_wifi_set_mode(WIFI_MODE_STA);
    if (have_creds) {
        /* No explicit esp_wifi_connect() here — see softap_apply_cb() for why
         * that raced WIFI_EVENT_STA_START's own connect() and reliably failed
         * to associate. The mode switch above is enough to trigger it once. */
        wifi_config_t cfg;
        fill_sta_config(&cfg);
        esp_wifi_set_config(WIFI_IF_STA, &cfg);
        s_retry_ms = RETRY_MIN_MS;
    } else {
        set_state(NET_DOWN);
    }
    ESP_LOGI(TAG, "setup AP stopped, resuming station mode");
}

/* ------------------------------------------------------------------ timezone */

void net_get_timezone(char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        size_t n = out_size;
        const esp_err_t err = nvs_get_str(h, NVS_KEY_TZ, out, &n);
        nvs_close(h);
        if (err == ESP_OK && out[0]) {
            return;
        }
    }
    snprintf(out, out_size, "%s", CONFIG_DESKOS_TZ);
}

esp_err_t net_set_timezone(const char *tz)
{
    if (!tz || !*tz) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h), TAG, "nvs open");
    esp_err_t err = nvs_set_str(h, NVS_KEY_TZ, tz);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        return err;
    }

    setenv("TZ", tz, 1);
    tzset();
    ESP_LOGI(TAG, "timezone set to '%s'", tz);
    return ESP_OK;
}

/* ------------------------------------------------------------------ events */

static void retry_task(void *arg)
{
    (void)arg;
    const uint32_t wait = s_retry_ms;
    vTaskDelay(pdMS_TO_TICKS(wait));

    if (s_state != NET_UP) {
        ESP_LOGI(TAG, "reconnecting after %lu ms", (unsigned long)wait);
        esp_wifi_connect();
    }
    vTaskDelete(NULL);
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)data;

    switch (id) {
    case WIFI_EVENT_STA_START:
        if (s_ssid[0]) {
            esp_wifi_connect();
            set_state(NET_CONNECTING);
        }
        break;

    case WIFI_EVENT_STA_DISCONNECTED:
        if (s_softap_active) {
            /* Expected: net_softap_start() just disconnected STA on purpose to
             * switch to AP mode. Reconnecting here would fight the setup flow. */
            break;
        }
        set_state(NET_CONNECTING);
        /* Exponential backoff so a missing access point does not spin the radio. */
        xTaskCreate(retry_task, "wifi_retry", 2048, NULL, 3, NULL);
        s_retry_ms = (s_retry_ms * 2 > RETRY_MAX_MS) ? RETRY_MAX_MS : s_retry_ms * 2;
        break;

    default:
        break;
    }
}

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)id;

    const ip_event_got_ip_t *ev = data;
    ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&ev->ip_info.ip));
    s_retry_ms = RETRY_MIN_MS;
    set_state(NET_UP);
}

static void on_sntp_sync(struct timeval *tv)
{
    (void)tv;
    if (!s_time_valid) {
        s_time_valid = true;
        time_t now = time(NULL);
        struct tm tm_now;
        localtime_r(&now, &tm_now);
        ESP_LOGI(TAG, "clock set: %04d-%02d-%02d %02d:%02d:%02d",
                 tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday,
                 tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
    }
}

/* ------------------------------------------------------------------- setup */

esp_err_t net_init(void)
{
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");
    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_cfg), TAG, "wifi init");

    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, NULL), TAG, "wifi handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, on_got_ip, NULL, NULL), TAG, "ip handler");

    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "storage");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "mode");

    if (load_credentials()) {
        wifi_config_t cfg;
        fill_sta_config(&cfg);
        ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &cfg), TAG, "set config");
        ESP_LOGI(TAG, "connecting to '%s'", s_ssid);
    } else {
        ESP_LOGW(TAG, "no WiFi credentials — set CONFIG_DESKOS_WIFI_SSID via "
                      "'idf.py menuconfig', or call net_set_credentials()");
    }

    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");

    char tz[48];
    net_get_timezone(tz, sizeof(tz));
    setenv("TZ", tz, 1);
    tzset();

    esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_DESKOS_SNTP_SERVER);
    sntp_cfg.start = true;
    sntp_cfg.server_from_dhcp = false;
    sntp_cfg.sync_cb = on_sntp_sync;
    ESP_RETURN_ON_ERROR(esp_netif_sntp_init(&sntp_cfg), TAG, "sntp init");

    return ESP_OK;
}

net_state_t net_state(void)
{
    return s_state;
}

bool net_time_valid(void)
{
    return s_time_valid;
}

void net_on_state_change(net_state_cb_t cb)
{
    s_cb = cb;
}
