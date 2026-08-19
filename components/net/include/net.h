/*
 * WiFi station + SNTP.
 *
 * Deliberately independent of the PC agent: the device must stay useful while
 * the PC is asleep, so anything a widget needs from the internet it fetches
 * itself. See CLAUDE.md.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NET_DOWN,          /* no credentials, or disconnected and waiting to retry */
    NET_CONNECTING,
    NET_UP,            /* associated and holding an IP */
} net_state_t;

typedef void (*net_state_cb_t)(net_state_t state);

/* Starts the WiFi stack and connects if credentials are available. Returns OK
 * even with no credentials configured — that is a normal state that the shell
 * reports rather than a boot failure. */
esp_err_t net_init(void);

net_state_t net_state(void);

/* True once SNTP has set the clock at least once. Widgets that show time must
 * check this instead of assuming 1970 means anything. */
bool net_time_valid(void);

/* Persists credentials to NVS and reconnects. Passing NULL/empty ssid clears
 * them. This is the runtime path; the build-time path is CONFIG_DESKOS_WIFI_*. */
esp_err_t net_set_credentials(const char *ssid, const char *password);

/* Copies the SSID currently in use. False when none is configured. */
bool net_get_ssid(char *out, size_t out_size);

/* Persists a POSIX TZ string to NVS and applies it immediately (setenv+tzset) —
 * takes effect on the very next localtime() call, no reboot needed. The
 * build-time fallback is CONFIG_DESKOS_TZ. See Kconfig.projbuild for the
 * "POSIX TZ counts backwards" warning before constructing one by hand. */
esp_err_t net_set_timezone(const char *tz);

/* Copies the TZ string currently in effect (NVS override or the build-time
 * default) into out. Always succeeds — there is always an effective TZ. */
void net_get_timezone(char *out, size_t out_size);

void net_on_state_change(net_state_cb_t cb);

#ifdef __cplusplus
}
#endif
