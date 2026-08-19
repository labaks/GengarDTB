#pragma once

#include <stdbool.h>
#include "cJSON.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HOST_DOWN,         /* no token configured, agent not found, or disconnected */
    HOST_CONNECTING,
    HOST_UP,           /* handshake done, subscriptions active */
} host_state_t;

typedef void (*host_state_cb_t)(host_state_t state);

/* Called with the topic name and its latest value on every "data" message for
 * a topic currently in the active subscription set. One slot, not a list:
 * at most one widget is ever open at a time (see widget.c), so there is only
 * ever one interested party. */
typedef void (*host_data_cb_t)(const char *topic, const cJSON *value);

esp_err_t host_init(void);

host_state_t host_state(void);
void         host_on_state_change(host_state_cb_t cb);
void         host_on_data(host_data_cb_t cb);

/* Replaces the active subscription outright (protocol: "subscribe" is not
 * cumulative) — pass count 0 to unsubscribe from everything. Safe to call
 * whether or not the client is currently connected; it is applied once (or
 * as soon as) the connection comes up. */
void host_set_topics(const char *const *topics, size_t count);

#ifdef __cplusplus
}
#endif
