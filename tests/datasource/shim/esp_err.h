/*
 * Stand-in for ESP-IDF's esp_err.h, just enough to parse datasource.h on a
 * host compiler. datasource_fetch_json (the only function that returns
 * esp_err_t) lives in datasource_http.c and is never linked in here — this
 * only needs to satisfy its declaration.
 */
#pragma once

typedef int esp_err_t;
