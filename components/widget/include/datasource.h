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

/*
 * Copies tmpl into out, replacing every {{...}} with its rendered value.
 * Unknown or missing paths render as "--" rather than failing: a widget with
 * one stale field should still show the rest.
 *
 * Inside {{...}}, a path can be followed by a pipeline of filters:
 *
 *   {{current.temperature_2m | round:0}} C
 *   {{current.weather_code | dict:weather_code}}
 *   {{current.apparent_temperature | sub:current.temperature_2m | round:1}}
 *
 * Filters, each optionally taking a ":arg":
 *   round:N   - fix the number to N decimal places (numbers only)
 *   add:X     - X is a literal number, or another path resolved against root
 *   sub:X     -   (same)
 *   mul:X     -   (same) - this is how a unit conversion reads: "| mul:0.621"
 *   div:X     -   (same), a zero divisor leaves the value untouched
 *   dict:NAME - map the value through a table registered by
 *               {"dict":"NAME","map":{"0":"Clear",...}} elsewhere in the
 *               same ui.jsonl; an unmapped key passes the value through
 *               unchanged rather than blanking it
 *   date:FMT  - reparse a "YYYY-MM-DD" or "YYYY-MM-DDTHH:MM" string and
 *               reformat it with strftime tokens (no weekday: the parse
 *               never computes tm_wday, so %a/%A are not meaningful here)
 *
 * An arithmetic filter or round on a non-numeric value, an unknown dict name,
 * or an unrecognised filter name are all no-ops: the value passes through as
 * it was, on the same "stale but visible" principle as a missing path.
 *
 * dicts is the table of named maps built from {"dict":...} lines; pass NULL
 * if the template has no dict filters.
 */
void datasource_render(const char *tmpl, const cJSON *root, const cJSON *dicts,
                       char *out, size_t out_size);

/* Truthiness for {"if": "path"} view visibility: missing, false, zero,
 * empty string, "0" and "false" are false; everything else, including a
 * non-empty array/object, is true. */
bool datasource_truthy(const cJSON *node);

#ifdef __cplusplus
}
#endif
