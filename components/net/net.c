#include "net.h"

#include <string.h>
#include <time.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
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

static bool load_credentials(void)
{
    s_ssid[0] = s_pass[0] = '\0';

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

esp_err_t net_set_credentials(const char *ssid, const char *password)
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

    if (err != ESP_OK) {
        return err;
    }

    /* Never log the password, and never log the SSID at anything but INFO. */
    ESP_LOGI(TAG, "credentials updated (ssid '%s')", ssid ? ssid : "<cleared>");

    esp_wifi_disconnect();
    if (load_credentials()) {
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
    esp_netif_create_default_wifi_sta();

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
