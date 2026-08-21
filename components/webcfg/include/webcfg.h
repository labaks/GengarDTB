/*
 * On-demand web form for every SD config file this device reads
 * (ROADMAP #45) — the direct follow-up to #34/#37/#38.4 each adding one
 * more file (icons' fetch.json, ota.json's manifest_url, ...) to a growing
 * pile that until now only a physical card swap could edit.
 *
 * Deliberately NOT a merge into one /sd/config.json (an earlier sketch of
 * #45 proposed that): net.c/host.c/ota.c/fetch.c each already have a
 * working, separately-tested reader for their own file, and rewriting all
 * four just to change where the bytes come from is real regression risk
 * for zero user-visible benefit — the actual ask ("never touch the card
 * again") is fully met by one form that reads and writes the *same* files
 * those readers already expect. Each field is written independently
 * (webcfg_save_field()): a field left blank on the form leaves that file
 * untouched rather than being cleared, so filling in just the WiFi section
 * cannot accidentally blank out an already-configured OTA URL.
 *
 * Toggled from Settings, not always running — this is one more
 * esp_http_server instance's worth of heap on a board with no PSRAM (see
 * CLAUDE.md's running heap budget), same reasoning that already made the
 * SoftAP WiFi-setup portal (net_softap_start(), #8) on-demand rather than
 * permanent. Runs on the existing STA connection instead of standing up its
 * own AP the way SoftAP does — this device already has an IP once WiFi
 * setup itself is done.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WEBCFG_PORT 8080

/* Starts the config HTTP server on the current STA IP (net_get_ip()) at
 * WEBCFG_PORT. Fails if WiFi is not currently up — there is no AP fallback
 * here, unlike net_softap_start(); if the network is down, so is every one
 * of the subsystems this is meant to configure. Safe to call again while
 * already running (no-op). */
esp_err_t webcfg_start(void);

void webcfg_stop(void);

bool webcfg_is_running(void);

#ifdef __cplusplus
}
#endif
