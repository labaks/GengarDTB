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
    NET_SOFTAP,        /* WiFi setup mode: STA is down, see net_softap_start() */
} net_state_t;

typedef void (*net_state_cb_t)(net_state_t state);

/* Fallback AP name and the fixed address esp_netif hands out to it — for the
 * setup screen to display, so the two ends of the flow don't drift apart. */
#define NET_SOFTAP_SSID "deskos-setup"
#define NET_SOFTAP_URL  "http://192.168.4.1"

/* The SSID net_softap_start() will actually use: /sd/device.json's "name"
 * (ROADMAP #38.2/#45) if set, else NET_SOFTAP_SSID. The WiFi setup screen's
 * own instructions need this instead of the bare constant, so they don't
 * tell the user to look for a network name that isn't the one the AP is
 * actually broadcasting. */
void net_get_softap_ssid(char *out, size_t out_size);

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

/* Copies the STA IP address as dotted-decimal text (e.g. "192.168.1.100").
 * False when not currently associated (net_state() != NET_UP) — there is no
 * address worth showing then. ROADMAP #45: the on-demand web config server
 * needs this to tell the user where to point a browser. */
bool net_get_ip(char *out, size_t out_size);

/* Persists a POSIX TZ string to NVS and applies it immediately (setenv+tzset) —
 * takes effect on the very next localtime() call, no reboot needed. The
 * build-time fallback is CONFIG_DESKOS_TZ. See Kconfig.projbuild for the
 * "POSIX TZ counts backwards" warning before constructing one by hand. */
esp_err_t net_set_timezone(const char *tz);

/* Copies the TZ string currently in effect (NVS override or the build-time
 * default) into out. Always succeeds — there is always an effective TZ. */
void net_get_timezone(char *out, size_t out_size);

void net_on_state_change(net_state_cb_t cb);

/* Brings up a temporary open SoftAP (NET_SOFTAP_SSID) with a one-page HTTP
 * form at NET_SOFTAP_URL for typing an SSID/password from a phone or laptop —
 * there is no keyboard on this device. STA disconnects while this is active.
 * Submitting the form applies the new credentials and returns to station mode
 * on its own; net_softap_stop() cancels and returns to the previous network.
 * Safe to call net_softap_stop() when setup mode is not active. */
esp_err_t net_softap_start(void);
void      net_softap_stop(void);

#ifdef __cplusplus
}
#endif
