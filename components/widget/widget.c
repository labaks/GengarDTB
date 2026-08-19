#include "widget.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "datasource.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host.h"
#include "lvgl.h"
#include "net.h"

static const char *TAG = "widget";

#define WIDGET_MAX_OBJS   24
#define WIDGET_MAX_UI     8192      /* a layer-A layout is a handful of lines */
#define WIDGET_TEXT_MAX   128
#define REFRESH_MIN_S     30        /* floor, so a typo cannot hammer an API   */

#define WIDGET_ERR_SHOWN  4         /* lines shown on screen; rest just counted */
#define WIDGET_ERR_BUF    512

#define WIDGET_CACHE_NAME ".cache.json"
#define WIDGET_MAX_CACHE  4096      /* combined source data, not the raw ui.jsonl */

typedef enum {
    WVIEW_LABEL,
    WVIEW_RECT,
    WVIEW_LINE,
    WVIEW_BAR,
    WVIEW_ARC,
    WVIEW_IMAGE,
} wview_kind_t;

typedef struct {
    lv_obj_t     *obj;
    wview_kind_t  kind;
    char         *tmpl;   /* NULL when static. LABEL: text. BAR/ARC: numeric
                            * value. IMAGE: file name, relative to the app's
                            * own directory. Unused by RECT/LINE. */
    char         *cond;   /* NULL when always visible. A bare path (no
                            * {{...}}, no filters); see datasource_truthy. */
} wobj_t;

static const app_info_t *s_app;
static lv_obj_t         *s_screen;
static lv_obj_t         *s_prev_screen;
static lv_obj_t         *s_status_label;
static wobj_t            s_objs[WIDGET_MAX_OBJS];
static size_t            s_nobjs;

/* Named lookup tables for the "dict" filter, e.g. {"dict":"weather_code",
 * "map":{"0":"Clear",...}}. One object: {name: map, name: map, ...}. */
static cJSON            *s_dicts;

/* lv_line_set_points() stores the pointer, not a copy — the array has to
 * outlive the object, so it cannot live on add_view()'s stack. */
static lv_point_precise_t s_line_pts[WIDGET_MAX_OBJS][2];

/* Each {"src":"http",...} line becomes one of these. An unnamed source (no
 * "as") spreads its fields at the document's top level — the shape every
 * single-source widget has always rendered against. A named one nests under
 * its own key, so {{weather.current...}} and {{fx.rates...}} can live in the
 * same widget at different poll periods without colliding. */
typedef struct {
    char     name[24];
    char     url[256];
    uint32_t every_s;
    uint32_t elapsed_s;
} wsource_t;

#define WIDGET_MAX_SOURCES 4

static wsource_t         s_sources[WIDGET_MAX_SOURCES];
static size_t            s_nsources;
static cJSON            *s_combined;   /* persists across fetches; each source
                                         * updates its own slice of it */
static bool              s_showing_cache;   /* true until the first live fetch lands */
static time_t            s_cache_ts;        /* epoch when the on-disk cache was written */

/* {"src":"host","topic":"cpu"} — always named (unlike http, "topic" is not
 * optional), pushed by the shared host-agent client (host.c, ROADMAP #13)
 * rather than polled. Not disk-cached: unlike an HTTP response, there is
 * nothing meaningful to show from a previous session once the agent is gone. */
#define WIDGET_MAX_HOST_TOPICS 4
static char               s_host_topics[WIDGET_MAX_HOST_TOPICS][24];
static size_t             s_nhosttopics;

static bool              s_has_clock;
static TaskHandle_t      s_task;
static volatile bool     s_stop;

/* A malformed ui.jsonl used to just log a warning UART nobody was watching
 * and quietly skip the line. That leaves a widget author staring at a blank
 * or half-drawn screen with no idea why. Every parse problem is collected
 * here too and shown on the widget's own screen once opening finishes. */
static char               s_err_buf[WIDGET_ERR_BUF];
static int                s_err_count;
static bool               s_err_overflow_reported;

static void report_error(int line_no, const char *fmt, ...)
{
    char msg[80];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    ESP_LOGW(TAG, "line %d: %s", line_no, msg);

    s_err_count++;
    if (s_err_count > WIDGET_ERR_SHOWN) {
        return;   /* still counted above, just not spelled out on screen */
    }
    char entry[128];
    snprintf(entry, sizeof(entry), "%sline %d: %s", s_err_buf[0] ? "\n" : "", line_no, msg);
    strncat(s_err_buf, entry, sizeof(s_err_buf) - strlen(s_err_buf) - 1);
}

/* ------------------------------------------------------------ jsonl parsing */

static const lv_font_t *font_for(int size)
{
    switch (size) {
    case 28: return &lv_font_montserrat_28;
    case 20: return &lv_font_montserrat_20;
    default: return &lv_font_montserrat_14;
    }
}

static uint32_t colour_for(const cJSON *node, uint32_t fallback)
{
    const char *s = cJSON_GetStringValue(node);
    if (!s) {
        return fallback;
    }
    return (uint32_t)strtoul(s, NULL, 0);   /* accepts 0xRRGGBB and plain hex */
}

/* BAR/ARC "value" is a {{path}} template like everything else, just fed
 * through atof() afterwards instead of straight onto a label. */
static void apply_numeric(lv_obj_t *obj, wview_kind_t kind, const char *tmpl, const cJSON *root)
{
    char rendered[32];
    datasource_render(tmpl, root, s_dicts, rendered, sizeof(rendered));
    const int32_t value = (int32_t)atof(rendered);
    if (kind == WVIEW_ARC) {
        lv_arc_set_value(obj, value);
    } else {
        lv_bar_set_value(obj, value, LV_ANIM_OFF);
    }
}

/* "A" is CONFIG_LV_FS_STDIO_LETTER (65) — LVGL's stdio fs driver strips the
 * "A:" prefix and fopen()s the rest, so this is just the app's own directory
 * with a drive letter glued on front. */
static void image_apply(lv_obj_t *obj, const char *filename)
{
    if (!obj || !filename || !*filename || !s_app) {
        return;
    }
    char path[192];
    snprintf(path, sizeof(path), "A:%s/%s", s_app->dir, filename);
    lv_image_set_src(obj, path);
}

static void add_view(const cJSON *line, int line_no)
{
    if (s_nobjs >= WIDGET_MAX_OBJS) {
        if (!s_err_overflow_reported) {
            s_err_overflow_reported = true;
            report_error(line_no, "too many views (max %d) — rest ignored", WIDGET_MAX_OBJS);
        }
        return;
    }

    const char *kind = cJSON_GetStringValue(cJSON_GetObjectItem(line, "obj"));
    if (!kind) {
        report_error(line_no, "missing 'obj'");
        return;
    }

    const int x = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(line, "x"));
    const int y = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(line, "y"));

    lv_obj_t *obj = NULL;

    wview_kind_t wkind = WVIEW_LABEL;

    if (strcmp(kind, "label") == 0) {
        obj = lv_label_create(s_screen);

        const cJSON *w = cJSON_GetObjectItem(line, "w");
        if (cJSON_IsNumber(w)) {
            lv_obj_set_width(obj, (int)cJSON_GetNumberValue(w));
            lv_label_set_long_mode(obj, LV_LABEL_LONG_WRAP);
        }

        lv_obj_set_style_text_font(obj,
            font_for((int)cJSON_GetNumberValue(cJSON_GetObjectItem(line, "font"))),
            LV_PART_MAIN);
        lv_obj_set_style_text_color(obj,
            lv_color_hex(colour_for(cJSON_GetObjectItem(line, "color"), 0xE8E8E8)),
            LV_PART_MAIN);

        const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(line, "text"));
        if (text) {
            /* Keep the template: refreshes re-render it against new data.
             * A string with no {{...}} simply renders to itself. */
            s_objs[s_nobjs].tmpl = strdup(text);

            char rendered[WIDGET_TEXT_MAX];
            datasource_render(text, NULL, s_dicts, rendered, sizeof(rendered));
            lv_label_set_text(obj, rendered);
        } else {
            lv_label_set_text(obj, "");
        }
    } else if (strcmp(kind, "rect") == 0) {
        wkind = WVIEW_RECT;
        obj = lv_obj_create(s_screen);
        lv_obj_set_size(obj,
            (int)cJSON_GetNumberValue(cJSON_GetObjectItem(line, "w")),
            (int)cJSON_GetNumberValue(cJSON_GetObjectItem(line, "h")));
        lv_obj_set_style_bg_color(obj,
            lv_color_hex(colour_for(cJSON_GetObjectItem(line, "color"), 0x202830)),
            LV_PART_MAIN);
        lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    } else if (strcmp(kind, "line") == 0) {
        /* A static divider, not a data-bound view: LVGL draws it from
         * (x1,y1)-(x2,y2), offset by the object's own (x,y). */
        wkind = WVIEW_LINE;
        obj = lv_line_create(s_screen);

        lv_point_precise_t *pts = s_line_pts[s_nobjs];
        pts[0].x = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(line, "x1"));
        pts[0].y = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(line, "y1"));
        pts[1].x = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(line, "x2"));
        pts[1].y = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(line, "y2"));
        lv_line_set_points(obj, pts, 2);

        lv_obj_set_style_line_color(obj,
            lv_color_hex(colour_for(cJSON_GetObjectItem(line, "color"), 0x445566)),
            LV_PART_MAIN);
        const cJSON *width = cJSON_GetObjectItem(line, "width");
        lv_obj_set_style_line_width(obj,
            cJSON_IsNumber(width) ? (int)cJSON_GetNumberValue(width) : 2, LV_PART_MAIN);
    } else if (strcmp(kind, "bar") == 0 || strcmp(kind, "arc") == 0) {
        const bool is_arc = (kind[0] == 'a');
        wkind = is_arc ? WVIEW_ARC : WVIEW_BAR;
        obj = is_arc ? lv_arc_create(s_screen) : lv_bar_create(s_screen);
        lv_obj_set_size(obj,
            (int)cJSON_GetNumberValue(cJSON_GetObjectItem(line, "w")),
            (int)cJSON_GetNumberValue(cJSON_GetObjectItem(line, "h")));

        const cJSON *min = cJSON_GetObjectItem(line, "min");
        const cJSON *max = cJSON_GetObjectItem(line, "max");
        const int range_min = cJSON_IsNumber(min) ? (int)cJSON_GetNumberValue(min) : 0;
        const int range_max = cJSON_IsNumber(max) ? (int)cJSON_GetNumberValue(max) : 100;

        if (is_arc) {
            lv_arc_set_range(obj, range_min, range_max);
            lv_arc_set_mode(obj, LV_ARC_MODE_NORMAL);
            /* This is a passive display, not a control: nothing on the
             * widget screen should be draggable under the resistive touch. */
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_arc_color(obj,
                lv_color_hex(colour_for(cJSON_GetObjectItem(line, "color"), 0x4A9EFF)),
                LV_PART_INDICATOR);
        } else {
            lv_bar_set_range(obj, range_min, range_max);
            lv_obj_set_style_bg_color(obj,
                lv_color_hex(colour_for(cJSON_GetObjectItem(line, "color"), 0x4A9EFF)),
                LV_PART_INDICATOR);
        }

        const char *value = cJSON_GetStringValue(cJSON_GetObjectItem(line, "value"));
        if (value) {
            /* Same reasoning as label's initial render below: a widget with
             * no data source (a plain "value":"42") never calls apply_data,
             * so the first render has to happen here or it never happens. */
            s_objs[s_nobjs].tmpl = strdup(value);
            apply_numeric(obj, wkind, value, NULL);
        }
    } else if (strcmp(kind, "image") == 0) {
        wkind = WVIEW_IMAGE;
        obj = lv_image_create(s_screen);

        const char *src = cJSON_GetStringValue(cJSON_GetObjectItem(line, "src"));
        if (src) {
            s_objs[s_nobjs].tmpl = strdup(src);

            char rendered[WIDGET_TEXT_MAX];
            datasource_render(src, NULL, s_dicts, rendered, sizeof(rendered));
            image_apply(obj, rendered);
        }
    } else {
        report_error(line_no, "unknown view type '%s'", kind);
        return;
    }

    lv_obj_set_pos(obj, x, y);

    const char *cond = cJSON_GetStringValue(cJSON_GetObjectItem(line, "if"));
    if (cond) {
        s_objs[s_nobjs].cond = strdup(cond);
        /* No root exists yet, so this always resolves false: a conditioned
         * view starts hidden and is corrected on the first refresh, same as
         * a data-bound label starts as "--" until then. */
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }

    s_objs[s_nobjs].obj = obj;
    s_objs[s_nobjs].kind = wkind;
    s_nobjs++;
}

static void parse_line(const char *line, int line_no)
{
    cJSON *root = cJSON_Parse(line);
    if (!root) {
        report_error(line_no, "malformed JSON, line skipped");
        return;
    }

    const char *dict_name = cJSON_GetStringValue(cJSON_GetObjectItem(root, "dict"));
    const cJSON *dict_map = cJSON_GetObjectItem(root, "map");

    if (dict_name && cJSON_IsObject(dict_map)) {
        if (!s_dicts) {
            s_dicts = cJSON_CreateObject();
        }
        cJSON_DeleteItemFromObject(s_dicts, dict_name);   /* redeclare = replace */
        cJSON_AddItemToObject(s_dicts, dict_name, cJSON_Duplicate(dict_map, true));
        cJSON_Delete(root);
        return;
    }

    const char *src = cJSON_GetStringValue(cJSON_GetObjectItem(root, "src"));
    if (src && strcmp(src, "http") == 0) {
        const char *url = cJSON_GetStringValue(cJSON_GetObjectItem(root, "url"));
        const char *as  = cJSON_GetStringValue(cJSON_GetObjectItem(root, "as"));
        if (!url) {
            report_error(line_no, "http source missing 'url'");
        } else if (s_nsources >= WIDGET_MAX_SOURCES) {
            report_error(line_no, "too many sources (max %d)", WIDGET_MAX_SOURCES);
        } else {
            wsource_t *s = &s_sources[s_nsources++];
            snprintf(s->name, sizeof(s->name), "%s", as ? as : "");
            snprintf(s->url, sizeof(s->url), "%s", url);
            const int every = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(root, "every"));
            s->every_s = (every < REFRESH_MIN_S) ? REFRESH_MIN_S : (uint32_t)every;
            s->elapsed_s = s->every_s;   /* due immediately on the scheduler's first tick */
        }
    } else if (src && strcmp(src, "clock") == 0) {
        /* Local source: no fetch, no network dependency. Ticks once a second
         * so a minute rollover shows up promptly instead of waiting out the
         * http floor above. */
        s_has_clock = true;
    } else if (src && strcmp(src, "host") == 0) {
        const char *topic = cJSON_GetStringValue(cJSON_GetObjectItem(root, "topic"));
        if (!topic || !*topic) {
            report_error(line_no, "host source missing 'topic'");
        } else if (s_nhosttopics >= WIDGET_MAX_HOST_TOPICS) {
            report_error(line_no, "too many host topics (max %d)", WIDGET_MAX_HOST_TOPICS);
        } else {
            snprintf(s_host_topics[s_nhosttopics], sizeof(s_host_topics[0]), "%s", topic);
            s_nhosttopics++;
        }
    } else {
        add_view(root, line_no);
    }

    cJSON_Delete(root);
}

/* -------------------------------------------------------------- refreshing */

static void apply_data(const cJSON *root)
{
    char rendered[WIDGET_TEXT_MAX];

    for (size_t i = 0; i < s_nobjs; i++) {
        if (!s_objs[i].obj) {
            continue;
        }

        if (s_objs[i].tmpl) {
            switch (s_objs[i].kind) {
            case WVIEW_BAR:
            case WVIEW_ARC:
                apply_numeric(s_objs[i].obj, s_objs[i].kind, s_objs[i].tmpl, root);
                break;
            case WVIEW_IMAGE:
                datasource_render(s_objs[i].tmpl, root, s_dicts, rendered, sizeof(rendered));
                image_apply(s_objs[i].obj, rendered);
                break;
            default:
                datasource_render(s_objs[i].tmpl, root, s_dicts, rendered, sizeof(rendered));
                lv_label_set_text(s_objs[i].obj, rendered);
                break;
            }
        }

        if (s_objs[i].cond) {
            if (datasource_truthy(datasource_resolve(root, s_objs[i].cond))) {
                lv_obj_remove_flag(s_objs[i].obj, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(s_objs[i].obj, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

static void set_status(const char *text)
{
    if (s_status_label) {
        lv_label_set_text(s_status_label, text);
    }
}

/* -------------------------------------------------------------- disk cache */

static void cache_path(char *out, size_t out_size)
{
    snprintf(out, out_size, "%s/%s", s_app->dir, WIDGET_CACHE_NAME);
}

/* Serializes s_combined — call this while it is safe to read (i.e. under the
 * same lock apply_data() needs, now that host.c's data callback can also be
 * touching s_combined on its own task). Just memory + CPU, no I/O, so holding
 * the LVGL lock for it is cheap. */
static char *build_cache_text(void)
{
    if (!s_combined) {
        return NULL;
    }
    cJSON *wrapper = cJSON_CreateObject();
    cJSON_AddNumberToObject(wrapper, "ts", (double)time(NULL));
    cJSON_AddItemToObject(wrapper, "data", cJSON_Duplicate(s_combined, true));

    char *text = cJSON_PrintUnformatted(wrapper);
    cJSON_Delete(wrapper);
    return text;
}

/* The actual flash write — deliberately separate from build_cache_text() so
 * callers can serialize under lock and write unlocked. Flash writes are slow
 * enough that doing this while holding the LVGL lock would visibly stutter
 * the UI. */
static void write_cache_file(const char *text)
{
    if (!text) {
        return;
    }
    char path[192];
    cache_path(path, sizeof(path));
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(text, 1, strlen(text), f);
        fclose(f);
        ESP_LOGI(TAG, "cache written: %s (%u bytes)", path, (unsigned)strlen(text));
    }
}

/* Loads the last good response(s) from a previous session, if any, so the
 * widget shows something the instant it opens instead of sitting blank until
 * the first fetch — the entire point when the PC is asleep or the router is
 * down. Autonomy is a hard requirement here (see CLAUDE.md), not a nicety. */
static bool cache_load(void)
{
    char path[192];
    cache_path(path, sizeof(path));

    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    /* Heap, not a stack array: this runs on whatever task called
     * widget_open() (the LVGL/input task), and 4KB on its stack is exactly
     * the kind of thing that silently corrupts the heap instead of failing
     * loudly — which is exactly what happened here on first hardware test. */
    char *buf = malloc(WIDGET_MAX_CACHE);
    if (!buf) {
        fclose(f);
        return false;
    }
    const size_t n = fread(buf, 1, WIDGET_MAX_CACHE - 1, f);
    fclose(f);
    buf[n] = '\0';

    cJSON *wrapper = cJSON_Parse(buf);
    free(buf);
    if (!wrapper) {
        return false;
    }
    const cJSON *data = cJSON_GetObjectItem(wrapper, "data");
    if (!cJSON_IsObject(data)) {
        cJSON_Delete(wrapper);
        return false;
    }

    s_cache_ts = (time_t)cJSON_GetNumberValue(cJSON_GetObjectItem(wrapper, "ts"));
    if (s_combined) {
        cJSON_Delete(s_combined);
    }
    s_combined = cJSON_Duplicate(data, true);
    cJSON_Delete(wrapper);
    ESP_LOGI(TAG, "cache loaded: %s (ts %ld)", path, (long)s_cache_ts);
    return true;
}

/* "cached" alone, honestly, when the age cannot be trusted: before the clock
 * has ever synced, or if it somehow moved backwards past the cache's own
 * timestamp. Showing a wrong number would be worse than showing none. */
static void format_cache_age(char *out, size_t out_size, time_t ts)
{
    if (ts <= 0 || !net_time_valid()) {
        snprintf(out, out_size, "cached");
        return;
    }
    const long age_s = (long)(time(NULL) - ts);
    if (age_s < 0) {
        snprintf(out, out_size, "cached");
    } else if (age_s < 60) {
        snprintf(out, out_size, "cached just now");
    } else if (age_s < 3600) {
        snprintf(out, out_size, "cached %ldm ago", age_s / 60);
    } else if (age_s < 86400) {
        snprintf(out, out_size, "cached %ldh ago", age_s / 3600);
    } else {
        snprintf(out, out_size, "cached %ldd ago", age_s / 86400);
    }
}

/* Folds one source's freshly-fetched body into the persistent combined
 * document, taking ownership of `fetched` either way. Unnamed sources spread
 * their fields at the top level — the exact shape a single-source widget has
 * always rendered against — so existing ui.jsonl files need no changes.
 * Named sources nest under their own key instead. Either way, a redeclare
 * (this source's previous fetch) is replaced, same convention as "dict". */
static void merge_source(const char *name, cJSON *fetched)
{
    if (!s_combined) {
        s_combined = cJSON_CreateObject();
    }
    if (name && *name) {
        cJSON_DeleteItemFromObject(s_combined, name);
        cJSON_AddItemToObject(s_combined, name, fetched);
        return;
    }
    cJSON *child = fetched->child;
    while (child) {
        cJSON *next = child->next;
        cJSON_DeleteItemFromObject(s_combined, child->string);
        cJSON_DetachItemViaPointer(fetched, child);
        cJSON_AddItemToObject(s_combined, child->string, child);
        child = next;
    }
    cJSON_Delete(fetched);   /* now an empty shell */
}

/* Decides what the corner says, in priority order: a stalled http source
 * outranks a stalled host connection (network trouble is the more actionable
 * fact), which outranks "everything's fine". Must be called under the LVGL
 * lock, same as apply_data(). */
static void update_status(bool fetched_any, bool all_ok)
{
    if (s_nsources > 0 && net_state() != NET_UP) {
        if (s_showing_cache) {
            /* Recomputed every call: the age keeps advancing (and the clock
             * may only just now have synced) while the network stays down. */
            char age[32];
            format_cache_age(age, sizeof(age), s_cache_ts);
            set_status(age);
        } else {
            set_status("waiting for network");
        }
        return;
    }
    if (s_nhosttopics > 0 && host_state() != HOST_UP) {
        set_status("waiting for pc");
        return;
    }
    set_status(fetched_any && !all_ok ? "stale" : "");
}

/* One tick per second, checking every http source's own period independently
 * — REFRESH_MIN_S floors each at 30s, so per-source overhead of a 1s poll is
 * negligible. Sleeping in 200ms slices (not one vTaskDelay(1000)) keeps
 * widget_close() from waiting up to a second for this task to notice s_stop.
 * Host topics need no polling here at all — host.c pushes them straight into
 * widget_host_data_cb() on its own; this task only keeps their status text
 * (and any http sources') current. */
static void refresh_task(void *arg)
{
    (void)arg;

    while (!s_stop) {
        bool fetched_any = false;
        bool any_success = false;
        bool all_ok = true;

        if (s_nsources > 0 && net_state() == NET_UP) {
            /* Fetch (slow, network-bound) before taking the lock; merge
             * (fast, in-memory) after. s_combined can also be written by
             * host.c's data callback on its own task, so mutating it has to
             * happen under the same lock apply_data() already needs — not
             * interleaved with the network round-trip. */
            cJSON *fetched[WIDGET_MAX_SOURCES] = { 0 };
            bool   ok[WIDGET_MAX_SOURCES] = { 0 };

            for (size_t i = 0; i < s_nsources; i++) {
                wsource_t *s = &s_sources[i];
                if (s->elapsed_s < s->every_s) {
                    continue;
                }
                s->elapsed_s = 0;
                fetched_any = true;

                const esp_err_t err = datasource_fetch_json(s->url, &fetched[i]);
                if (err == ESP_OK && fetched[i]) {
                    ok[i] = true;
                    any_success = true;
                } else {
                    /* Keep the last values on screen and say they are stale.
                     * Blanking the widget on a transient network hiccup is
                     * strictly worse than showing slightly old numbers. */
                    all_ok = false;
                    if (fetched[i]) {
                        cJSON_Delete(fetched[i]);
                    }
                }
            }

            char *cache_text = NULL;
            if (fetched_any && lvgl_port_lock(500)) {
                for (size_t i = 0; i < s_nsources; i++) {
                    if (ok[i]) {
                        merge_source(s_sources[i].name, fetched[i]);
                    }
                }
                if (any_success) {
                    cache_text = build_cache_text();
                    s_showing_cache = false;
                }
                apply_data(s_combined);
                update_status(fetched_any, all_ok);
                lvgl_port_unlock();
            }
            /* Flash write stays outside the lock — see write_cache_file(). */
            write_cache_file(cache_text);
            cJSON_free(cache_text);
        }

        if (!fetched_any && lvgl_port_lock(200)) {
            update_status(false, true);
            lvgl_port_unlock();
        }

        for (uint32_t i = 0; i < 5 && !s_stop; i++) {
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        for (size_t i = 0; i < s_nsources; i++) {
            s_sources[i].elapsed_s++;
        }
    }

    s_task = NULL;
    vTaskDelete(NULL);
}

/* Pushed by host.c on its own task whenever a subscribed topic updates.
 * `value` is a borrowed pointer valid only for this call, so it is duplicated
 * before merge_source() takes ownership. */
static void widget_host_data_cb(const char *topic, const cJSON *value)
{
    if (!lvgl_port_lock(200)) {
        return;
    }
    merge_source(topic, cJSON_Duplicate(value, true));
    apply_data(s_combined);
    lvgl_port_unlock();
}

/* Local source: no network, no fetch — just the device's own clock, which
 * keeps ticking whether or not WiFi is up. Weekday/month names render in
 * English abbreviations (newlib's "C" locale); see CLAUDE.md on Cyrillic. */
static void clock_task(void *arg)
{
    (void)arg;

    while (!s_stop) {
        cJSON *root = cJSON_CreateObject();
        const bool valid = net_time_valid();

        if (valid) {
            time_t now = time(NULL);
            struct tm tm_local;
            localtime_r(&now, &tm_local);

            char buf[16];
            strftime(buf, sizeof(buf), "%H:%M", &tm_local);
            cJSON_AddStringToObject(root, "time", buf);
            strftime(buf, sizeof(buf), "%d %b %Y", &tm_local);
            cJSON_AddStringToObject(root, "date", buf);
            strftime(buf, sizeof(buf), "%a", &tm_local);
            cJSON_AddStringToObject(root, "weekday", buf);
        }

        if (lvgl_port_lock(500)) {
            apply_data(root);
            set_status(valid ? "" : "waiting for time sync");
            lvgl_port_unlock();
        }
        cJSON_Delete(root);

        for (uint32_t i = 0; i < 5 && !s_stop; i++) {
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }

    s_task = NULL;
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------- public */

esp_err_t widget_open(const app_info_t *app)
{
    if (!app) {
        return ESP_ERR_INVALID_ARG;
    }
    widget_close();

    char path[160];
    snprintf(path, sizeof(path), "%s/%s", app->dir, app->entry);

    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "cannot open %s", path);

        /* Still show something rather than leaving the launcher looking like
         * the button press did nothing — this is exactly the manifest typo
         * (wrong "entry" file name) this task exists to surface. */
        s_app = app;
        s_prev_screen = lv_screen_active();
        s_screen = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(s_screen, lv_color_hex(0x101418), LV_PART_MAIN);
        lv_obj_remove_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *err = lv_label_create(s_screen);
        lv_obj_set_style_text_color(err, lv_color_hex(0xFFD24A), LV_PART_MAIN);
        lv_obj_set_width(err, LV_PCT(90));
        lv_label_set_long_mode(err, LV_LABEL_LONG_WRAP);
        lv_label_set_text_fmt(err, "Cannot open \"%s\"\n(check \"entry\" in manifest.json)",
                               app->entry);
        lv_obj_center(err);

        lv_screen_load(s_screen);
        return ESP_ERR_NOT_FOUND;
    }

    char *buf = malloc(WIDGET_MAX_UI + 1);
    if (!buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    const size_t n = fread(buf, 1, WIDGET_MAX_UI, f);
    fclose(f);
    buf[n] = '\0';

    s_app = app;
    s_nobjs = 0;
    s_nsources = 0;
    s_showing_cache = false;
    s_cache_ts = 0;
    s_nhosttopics = 0;
    s_has_clock = false;
    memset(s_objs, 0, sizeof(s_objs));
    s_err_buf[0] = '\0';
    s_err_count = 0;
    s_err_overflow_reported = false;

    s_prev_screen = lv_screen_active();
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(0x101418), LV_PART_MAIN);
    lv_obj_remove_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    /* Status corner, owned by the runtime rather than the widget: the user must
     * always be able to tell fresh data from stale, whatever the app declares. */
    s_status_label = lv_label_create(s_screen);
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(0x8899A6), LV_PART_MAIN);
    lv_obj_align(s_status_label, LV_ALIGN_BOTTOM_RIGHT, -4, -4);
    lv_label_set_text(s_status_label, "");

    /* Split on '\n' only (not strtok_r's usual "\r\n" delimiter SET) so that a
     * genuinely blank line still advances the count — strtok_r collapses runs
     * of delimiter characters, which would silently throw off every line
     * number reported after the first blank line in the file. */
    int line_no = 0;
    char *p = buf;
    while (*p) {
        line_no++;
        char *line = p;
        char *nl = strchr(p, '\n');
        if (nl) {
            *nl = '\0';
            p = nl + 1;
        } else {
            p += strlen(p);
        }
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\r') {
            line[len - 1] = '\0';
        }
        while (*line == ' ' || *line == '\t') {
            line++;
        }
        if (*line == '\0' || *line == '#') {
            continue;
        }
        parse_line(line, line_no);
    }
    free(buf);

    if (s_err_count > 0) {
        if (s_err_count > WIDGET_ERR_SHOWN) {
            char more[24];
            snprintf(more, sizeof(more), "\n+%d more", s_err_count - WIDGET_ERR_SHOWN);
            strncat(s_err_buf, more, sizeof(s_err_buf) - strlen(s_err_buf) - 1);
        }

        /* A banner, not a corner note: a widget author needs to notice this,
         * not squint for it. Sits just under the ~16px every ui.jsonl author
         * is already told to reserve for the global toolbar (shell-navigation.md). */
        lv_obj_t *err_label = lv_label_create(s_screen);
        lv_obj_set_style_text_color(err_label, lv_color_hex(0xFFD24A), LV_PART_MAIN);
        lv_obj_set_style_bg_color(err_label, lv_color_hex(0x402000), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(err_label, LV_OPA_80, LV_PART_MAIN);
        lv_obj_set_style_pad_all(err_label, 4, LV_PART_MAIN);
        lv_obj_set_style_text_font(err_label, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_set_width(err_label, LV_PCT(96));
        lv_label_set_long_mode(err_label, LV_LABEL_LONG_WRAP);
        lv_label_set_text_fmt(err_label, "ui.jsonl: %d issue%s\n%s",
                               s_err_count, s_err_count == 1 ? "" : "s", s_err_buf);
        lv_obj_align(err_label, LV_ALIGN_TOP_MID, 0, 18);
    }

    lv_screen_load(s_screen);
    ESP_LOGI(TAG, "opened '%s': %u view(s), %u source(s), %u host topic(s), %d parse issue(s)%s",
             app->id, (unsigned)s_nobjs, (unsigned)s_nsources, (unsigned)s_nhosttopics, s_err_count,
             s_has_clock ? ", clock source" : (s_nsources || s_nhosttopics ? "" : ", static"));

    /* Clock is still an alternative to everything else, not layered: it is
     * the one source that does not depend on the network at all, so a
     * manifest declaring it alongside http/host sources gets the clock.
     * Multiple http sources (ROADMAP #10) and http + host together are fine
     * — both just feed merge_source() into the same s_combined. */
    if (s_has_clock) {
        s_stop = false;
        if (xTaskCreatePinnedToCore(clock_task, "wdg_clock", 4096, NULL, 3, &s_task, 1)
                != pdPASS) {
            ESP_LOGE(TAG, "cannot start clock task");
            s_task = NULL;
        }
    } else if (s_nsources > 0 || s_nhosttopics > 0) {
        /* Show yesterday's numbers immediately rather than sit blank until
         * the first fetch lands — the device must stay useful with the PC
         * asleep or the router down (see CLAUDE.md's autonomy rule). Without
         * a cache the corner would otherwise sit blank for up to the ~10s
         * TLS connect timeout, indistinguishable from "nothing to report". */
        if (s_nsources > 0 && cache_load()) {
            apply_data(s_combined);
            s_showing_cache = true;
            char age[32];
            format_cache_age(age, sizeof(age), s_cache_ts);
            set_status(age);
        } else {
            set_status("loading");
        }

        if (s_nhosttopics > 0) {
            const char *topics[WIDGET_MAX_HOST_TOPICS];
            for (size_t i = 0; i < s_nhosttopics; i++) {
                topics[i] = s_host_topics[i];
            }
            host_on_data(widget_host_data_cb);
            host_set_topics(topics, s_nhosttopics);
        }

        s_stop = false;
        if (xTaskCreatePinnedToCore(refresh_task, "wdg_refresh", 6144, NULL, 3, &s_task, 0)
                != pdPASS) {
            ESP_LOGE(TAG, "cannot start refresh task");
            s_task = NULL;
        }
    }
    return ESP_OK;
}

void widget_close(void)
{
    if (s_nhosttopics > 0) {
        /* Stop future pushes before anything else is torn down. A push
         * already in flight when this runs may still land after — a
         * one-time no-op at worst (see widget_host_data_cb()), not a crash,
         * given at most one widget is ever open. */
        host_on_data(NULL);
        host_set_topics(NULL, 0);
        s_nhosttopics = 0;
    }

    if (s_task) {
        s_stop = true;
        /* The task touches LVGL, so it must not be waited on while we hold the
         * lock. Release it for the duration of the handshake. */
        lvgl_port_unlock();
        for (int i = 0; i < 100 && s_task; i++) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        lvgl_port_lock(0);
    }

    for (size_t i = 0; i < s_nobjs; i++) {
        free(s_objs[i].tmpl);
        free(s_objs[i].cond);
        s_objs[i].tmpl = NULL;
        s_objs[i].cond = NULL;
    }
    s_nobjs = 0;

    if (s_dicts) {
        cJSON_Delete(s_dicts);
        s_dicts = NULL;
    }
    if (s_combined) {
        cJSON_Delete(s_combined);
        s_combined = NULL;
    }
    s_nsources = 0;

    if (s_screen) {
        if (s_prev_screen) {
            lv_screen_load(s_prev_screen);
        }
        lv_obj_delete(s_screen);
        s_screen = NULL;
    }
    s_status_label = NULL;
    s_app = NULL;
}

bool widget_is_open(void)
{
    return s_screen != NULL;
}

const app_info_t *widget_current(void)
{
    return s_app;
}
