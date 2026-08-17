/*
 * Layer A data plumbing: fetch JSON over HTTP, resolve dotted paths into it,
 * and substitute {{path}} placeholders in text.
 *
 * No code from the app ever executes on the device. A layer-A widget is a
 * layout plus a description of where its numbers come from, which is what keeps
 * it affordable on a board with no PSRAM.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "cJSON.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Hard ceiling on a response body. Layer-A sources are small JSON documents;
 * anything larger is a misconfiguration, and we would rather refuse than spend
 * the heap finding out. */
#define DATASOURCE_MAX_BODY (12 * 1024)

/* Blocking GET. On success *out_json owns a parsed document the caller must
 * cJSON_Delete. Must not run on the LVGL task. */
esp_err_t datasource_fetch_json(const char *url, cJSON **out_json);

/* Resolves "current.temperature_2m" or "daily.temperature_2m_max.0" against
 * root. Numeric path components index arrays. NULL when the path is absent. */
const cJSON *datasource_resolve(const cJSON *root, const char *path);

/* Copies tmpl into out, replacing every {{path}} with its resolved value.
 * Unknown or missing paths render as "--" rather than failing: a widget with
 * one stale field should still show the rest. */
void datasource_render(const char *tmpl, const cJSON *root, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif
