/*
 * PC-agent client — protocol in docs/host-protocol.md (ROADMAP #12/#13).
 *
 * Device is the WebSocket client, agent is the server: same
 * reconnect-with-backoff shape already used for WiFi (net.c) and HTTP fetch
 * retry (widget.c), reused here via esp_websocket_client's own built-in
 * auto-reconnect rather than reinventing it.
 */
#include "host.h"

#include <stdio.h>
#include <string.h>

#include "bsp_pins.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_websocket_client.h"
#include "esp_transport_ws.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mdns.h"

static const char *TAG = "host";

#define HOST_MDNS_SERVICE    "_deskos-agent"
#define HOST_MDNS_PROTO      "_tcp"
#define HOST_PROTO_VERSION   1
#define HOST_MAX_TOPICS      4
#define HOST_TOPIC_LEN       24
#define HOST_TOKEN_LEN       64
#define HOST_MSG_MAX         1024   /* one JSON message; generous for the small payloads in the protocol doc */
#define HOST_FAIL_THRESHOLD  5      /* consecutive failed (re)connects before dropping the cached address */
#define HOST_SUPERVISOR_MS   3000

static host_state_t    s_state;
static host_state_cb_t s_state_cb;
static host_data_cb_t  s_data_cb;

static char             s_topics[HOST_MAX_TOPICS][HOST_TOPIC_LEN];
static size_t           s_ntopics;
static SemaphoreHandle_t s_topics_mutex;

static char s_token[HOST_TOKEN_LEN];
static char s_device_id[18];   /* "aa:bb:cc:dd:ee:ff" */

static esp_websocket_client_handle_t s_client;
static volatile bool s_fatal_error;   /* set by the WS event handler, acted on by
                                        * the supervisor task — stop()/destroy()
                                        * cannot be called from the handler itself */
static volatile int  s_fail_streak;

/* Heap, not a stack array: this is read from inside the WS client's own event
 * task. widget.c's disk-cache bug (a 4KB stack array corrupting the heap on a
 * 6KB task stack) is exactly the mistake this avoids — see ROADMAP #11. */
static char  *s_rx_buf;
static int    s_rx_len;

/* ------------------------------------------------------------- config i/o */

/* {"token":"..."} on the microSD card, same convention as /sd/wifi.json
 * (CLAUDE.md, ROADMAP #8): the file is the easiest way to pair a device with
 * an agent, or re-pair it, without a rebuild. No file or empty token means
 * the host feature simply stays idle — same as WiFi with no credentials. */
static bool load_token(void)
{
    char path[64];
    snprintf(path, sizeof(path), "%s/agent.json", BSP_SD_MOUNT_POINT);

    FILE *f = fopen(path, "rb");
    if (!f) {
        s_token[0] = '\0';
        return false;
    }
    char buf[160];
    const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    cJSON *root = cJSON_Parse(buf);
    bool ok = false;
    if (root) {
        const char *tok = cJSON_GetStringValue(cJSON_GetObjectItem(root, "token"));
        if (tok && *tok) {
            snprintf(s_token, sizeof(s_token), "%s", tok);
            ok = true;
        }
        cJSON_Delete(root);
    }
    if (!ok) {
        s_token[0] = '\0';
    }
    return ok;
}

/* ------------------------------------------------------------------ state */

static void set_state(host_state_t st)
{
    if (s_state == st) {
        return;
    }
    s_state = st;
    ESP_LOGI(TAG, "state -> %s", st == HOST_UP ? "UP" : st == HOST_CONNECTING ? "CONNECTING" : "DOWN");
    if (s_state_cb) {
        s_state_cb(st);
    }
}

/* --------------------------------------------------------------- sending */

static void send_hello(void)
{
    char msg[192];
    snprintf(msg, sizeof(msg),
             "{\"type\":\"hello\",\"proto\":%d,\"device_id\":\"%s\",\"token\":\"%s\"}",
             HOST_PROTO_VERSION, s_device_id, s_token);
    esp_websocket_client_send_text(s_client, msg, (int)strlen(msg), pdMS_TO_TICKS(2000));
}

/* "subscribe" replaces the active set outright (see host_set_topics()) —
 * topic names are our own config, not escaped, same assumption as the
 * device_id/token above. */
static void send_subscribe(void)
{
    char msg[HOST_MAX_TOPICS * HOST_TOPIC_LEN + 64];
    size_t off = (size_t)snprintf(msg, sizeof(msg), "{\"type\":\"subscribe\",\"topics\":[");

    if (xSemaphoreTake(s_topics_mutex, pdMS_TO_TICKS(500))) {
        for (size_t i = 0; i < s_ntopics && off < sizeof(msg); i++) {
            off += (size_t)snprintf(msg + off, sizeof(msg) - off, "%s\"%s\"", i ? "," : "", s_topics[i]);
        }
        xSemaphoreGive(s_topics_mutex);
    }
    snprintf(msg + off, sizeof(msg) - off, "]}");
    esp_websocket_client_send_text(s_client, msg, (int)strlen(msg), pdMS_TO_TICKS(2000));
}

/* ------------------------------------------------------------- receiving */

static void handle_message(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        ESP_LOGW(TAG, "malformed message from agent, ignored");
        return;
    }

    const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(root, "type"));
    if (type && strcmp(type, "hello_ack") == 0) {
        s_fail_streak = 0;
        set_state(HOST_UP);
        send_subscribe();
    } else if (type && strcmp(type, "error") == 0) {
        const char *code = cJSON_GetStringValue(cJSON_GetObjectItem(root, "code"));
        ESP_LOGW(TAG, "agent rejected us: %s", code ? code : "?");
        s_fatal_error = true;
    } else if (type && strcmp(type, "data") == 0) {
        const char *topic = cJSON_GetStringValue(cJSON_GetObjectItem(root, "topic"));
        const cJSON *value = cJSON_GetObjectItem(root, "value");
        if (topic && value && s_data_cb) {
            s_data_cb(topic, value);
        }
    }
    /* "subscribed" is informational only for now — nothing to act on yet. */

    cJSON_Delete(root);
}

static void ws_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)arg; (void)base;
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        s_rx_len = 0;
        set_state(HOST_CONNECTING);
        send_hello();
        break;

    case WEBSOCKET_EVENT_DATA:
        if (data->op_code != WS_TRANSPORT_OPCODES_TEXT) {
            break;   /* control frames are handled internally by the client */
        }
        if (data->payload_offset == 0) {
            s_rx_len = 0;
        }
        if (s_rx_len >= 0 && s_rx_len + data->data_len < HOST_MSG_MAX - 1) {
            memcpy(s_rx_buf + s_rx_len, data->data_ptr, (size_t)data->data_len);
            s_rx_len += data->data_len;
        } else {
            s_rx_len = -1;   /* overflow: drop the rest of this message */
        }
        if (data->payload_offset + data->data_len >= data->payload_len) {
            if (s_rx_len >= 0) {
                s_rx_buf[s_rx_len] = '\0';
                handle_message(s_rx_buf);
            } else {
                ESP_LOGW(TAG, "message from agent exceeded %d bytes, dropped", HOST_MSG_MAX);
            }
            s_rx_len = 0;
        }
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
    case WEBSOCKET_EVENT_ERROR:
        s_fail_streak++;
        set_state(HOST_DOWN);
        break;

    default:
        break;
    }
}

/* ------------------------------------------------------------- lifecycle */

static void teardown_client(void)
{
    if (!s_client) {
        return;
    }
    esp_websocket_client_stop(s_client);
    esp_websocket_client_destroy(s_client);
    s_client = NULL;
    set_state(HOST_DOWN);
}

/* Runs forever: reads the token, resolves the agent via mDNS, and starts the
 * WS client. The client's own auto-reconnect (see esp_websocket_client.h)
 * handles brief drops on its own; this loop only steps in to drop a stale
 * connection after HOST_FAIL_THRESHOLD straight failures (the agent's IP may
 * have changed) or after the agent has actively rejected us (hello error). */
static void supervisor_task(void *arg)
{
    (void)arg;

    for (;;) {
        if (!load_token()) {
            teardown_client();
            vTaskDelay(pdMS_TO_TICKS(HOST_SUPERVISOR_MS));
            continue;
        }

        if (s_fatal_error) {
            s_fatal_error = false;
            teardown_client();
        }

        if (s_client && s_fail_streak >= HOST_FAIL_THRESHOLD) {
            ESP_LOGW(TAG, "giving up on the cached address after %d failures, re-resolving", s_fail_streak);
            teardown_client();
        }

        if (!s_client) {
            mdns_result_t *results = NULL;
            if (mdns_query_ptr(HOST_MDNS_SERVICE, HOST_MDNS_PROTO, 3000, 1, &results) == ESP_OK && results) {
                esp_ip4_addr_t ip4 = {0};
                bool have_ip = false;
                for (mdns_ip_addr_t *a = results->addr; a; a = a->next) {
                    if (a->addr.type == ESP_IPADDR_TYPE_V4) {
                        ip4 = a->addr.u_addr.ip4;
                        have_ip = true;
                        break;
                    }
                }

                if (have_ip && results->port) {
                    char uri[48];
                    snprintf(uri, sizeof(uri), "ws://" IPSTR ":%u/deskos", IP2STR(&ip4), (unsigned)results->port);

                    const esp_websocket_client_config_t cfg = {
                        .uri = uri,
                        .reconnect_timeout_ms = 5000,
                        .network_timeout_ms = 8000,
                    };
                    s_client = esp_websocket_client_init(&cfg);
                    if (s_client) {
                        esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY, ws_event_handler, NULL);
                        s_fail_streak = 0;
                        set_state(HOST_CONNECTING);
                        ESP_LOGI(TAG, "found agent at %s", uri);
                        esp_websocket_client_start(s_client);
                    }
                } else {
                    ESP_LOGW(TAG, "agent found via mDNS but has no usable address/port");
                }
                mdns_query_results_free(results);
            }
            /* No agent found: normal when the PC is asleep or the agent
             * isn't running — just try again next pass, no error to log. */
        }

        vTaskDelay(pdMS_TO_TICKS(HOST_SUPERVISOR_MS));
    }
}

/* ------------------------------------------------------------------- public */

esp_err_t host_init(void)
{
    s_topics_mutex = xSemaphoreCreateMutex();
    s_rx_buf = malloc(HOST_MSG_MAX);
    if (!s_rx_buf || !s_topics_mutex) {
        return ESP_ERR_NO_MEM;
    }

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_device_id, sizeof(s_device_id), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    ESP_RETURN_ON_ERROR(mdns_init(), TAG, "mdns init");

    if (xTaskCreatePinnedToCore(supervisor_task, "host_sup", 4096, NULL, 3, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "cannot start supervisor task");
        return ESP_FAIL;
    }
    return ESP_OK;
}

host_state_t host_state(void)
{
    return s_state;
}

void host_on_state_change(host_state_cb_t cb)
{
    s_state_cb = cb;
}

void host_on_data(host_data_cb_t cb)
{
    s_data_cb = cb;
}

void host_set_topics(const char *const *topics, size_t count)
{
    if (count > HOST_MAX_TOPICS) {
        count = HOST_MAX_TOPICS;
    }
    if (xSemaphoreTake(s_topics_mutex, portMAX_DELAY)) {
        s_ntopics = count;
        for (size_t i = 0; i < count; i++) {
            snprintf(s_topics[i], HOST_TOPIC_LEN, "%s", topics[i]);
        }
        xSemaphoreGive(s_topics_mutex);
    }
    if (s_state == HOST_UP && s_client) {
        send_subscribe();
    }
}
