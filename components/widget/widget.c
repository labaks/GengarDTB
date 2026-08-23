#include "widget.h"

#include <stdarg.h>
#include <stdint.h>
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
#include "nvs.h"

static const char *TAG = "widget";

/* Same "define it locally, don't share a header" convention as every other
 * file that touches NVS (settings.c, net.c, bsp_display.c, bsp_touch.c). */
#define NVS_NAMESPACE "deskos"

#define WIDGET_MAX_OBJS   56      /* bumped for #40's per-cell weather grid
                                     * (title+icon+value per hour/day, 7 of
                                     * each); still cheap — wobj_t and its
                                     * line-points twin are small static
                                     * arrays. */
#define WIDGET_MAX_UI     10240     /* was 8192; #40's per-cell weather grid
                                       (49 lines) needs the extra room. Heap,
                                       freed at the end of widget_open() —
                                       transient, not held for the session. */
#define WIDGET_TEXT_MAX   128
#define REFRESH_MIN_S     30        /* floor, so a typo cannot hammer an API   */

#define WIDGET_ERR_SHOWN  4         /* lines shown on screen; rest just counted */
#define WIDGET_ERR_BUF    512

#define WIDGET_CACHE_NAME ".cache.json"
#define WIDGET_MAX_CACHE  4096      /* combined source data, not the raw ui.jsonl */
#define WIDGET_CACHE_MAX_AGE_S (3600)  /* a disk cache older than this is not
                                         * "yesterday's data", it's just wrong
                                         * — better to show "loading" and wait
                                         * for a real fetch than a stale value
                                         * dressed up as current */

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
    int           page;   /* -1: shown on every page (the default — existing
                            * ui.jsonl files declare no "page" at all and stay
                            * unaffected). >=0: shown only while that page is
                            * the active one, see s_page/s_max_page below. */
    char         *tap;    /* NULL when not tappable. An action name, same
                            * small fixed set dispatch_tap_action() knows —
                            * an unrecognised one is a silent no-op, same
                            * policy as an unknown filter name. */
} wobj_t;

static const app_info_t *s_app;
static lv_obj_t         *s_screen;
static lv_obj_t         *s_prev_screen;
static lv_obj_t         *s_status_label;
static wobj_t            s_objs[WIDGET_MAX_OBJS];
static size_t            s_nobjs;

/* Multi-page layout: views tagged with an explicit "page" only show while
 * s_page matches; untagged views (page == -1) show on every page, the usual
 * case for shared chrome like a header. s_max_page tracks the highest page
 * number any view declared, so "next_page" has something to wrap around at
 * — a widget that never uses "page" keeps s_max_page at 0 and next_page is
 * then a permanent no-op, not a crash or an out-of-range page. */
static int               s_page;
static int               s_max_page;

/* Persisted across reopen/reboot (NVS key "pg_<app id>") whenever a widget
 * declares more than one page — ROADMAP #39's style presets are pages like
 * any other (see clock.ui.jsonl), and a preset choice that reverted every
 * time the widget was reopened would not read as a real setting. Weather's
 * hourly/daily toggle (#40) rides along for free, which is harmless: nothing
 * about "page" was ever meant to be session-only, it just had no other
 * consumer needing persistence until now. */
static void page_nvs_key(const app_info_t *app, char *out, size_t out_size)
{
    /* NVS keys are capped at 15 chars total; "pg_" (3) + 11 of the id stays
     * safely under that for any id this project's app ids ever use. */
    snprintf(out, out_size, "pg_%.11s", app->id);
}

static int page_nvs_load(const app_info_t *app)
{
    char key[16];
    page_nvs_key(app, key, sizeof(key));
    nvs_handle_t h;
    int32_t v = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_i32(h, key, &v);
        nvs_close(h);
    }
    return (int)v;
}

static void page_nvs_save(const app_info_t *app, int page)
{
    char key[16];
    page_nvs_key(app, key, sizeof(key));
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, key, page);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* The clock source's own display preference — not per-widget like the page
 * above: any ui.jsonl that declares {"src":"clock"} shares one "how the user
 * likes to read time" setting, the same way the device only has one
 * timezone. Loaded lazily on first use (widget_open(), guarded by
 * s_has_clock, or the first Settings → Display call to widget_clock_h24()
 * below — whichever happens first this boot) rather than at boot: nothing
 * needs it before a clock source or that screen actually exists.
 *
 * ROADMAP #39's first attempt put the toggle on the clock face itself (a
 * tappable corner badge) — worked logically, but the resistive touch panel's
 * known imprecision (see CLAUDE.md on touch calibration) made a small target
 * genuinely hard to hit, confirmed on hardware. Moved to a real Settings row
 * instead (settings.c's Display view, a plain switch — see build_display()),
 * which is why the getter/setter below are public: settings.c owns the UI,
 * this file still owns the value and its persistence, since a pinned clock
 * widget can sit open on Home while the setting changes underneath it and
 * has to pick up the new value on its very next tick, not on next reopen. */
#define NVS_KEY_CLOCK_H24 "clock_h24"
static bool               s_clock_h24 = true;
static bool               s_clock_h24_loaded;

static void clock_format_load(void)
{
    if (s_clock_h24_loaded) {
        return;
    }
    s_clock_h24_loaded = true;
    nvs_handle_t h;
    uint8_t v = 1;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, NVS_KEY_CLOCK_H24, &v);
        nvs_close(h);
    }
    s_clock_h24 = v != 0;
}

bool widget_clock_h24(void)
{
    clock_format_load();
    return s_clock_h24;
}

void widget_set_clock_h24(bool h24)
{
    s_clock_h24_loaded = true;
    if (h24 == s_clock_h24) {
        return;
    }
    s_clock_h24 = h24;
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, NVS_KEY_CLOCK_H24, s_clock_h24 ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
}

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
    char     url[384];   /* #40's hourly+daily weather URL runs ~290 chars
                           * post-templating; 256 silently truncated it into
                           * a malformed query string (server-side HTTP 400,
                           * not a truncation error on our own end — nothing
                           * here checks datasource_render()'s output length) */
    uint32_t every_s;
    uint32_t elapsed_s;
} wsource_t;

#define WIDGET_MAX_SOURCES 4

static wsource_t         s_sources[WIDGET_MAX_SOURCES];
static size_t            s_nsources;
static cJSON            *s_combined;   /* persists across fetches; each source
                                         * updates its own slice of it */
static cJSON            *s_config;     /* "data"/"config" fields only — kept
                                         * apart so they can be reasserted over
                                         * a disk cache that may predate them,
                                         * see cache_load()'s own comment */
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
    case 48: return &lv_font_montserrat_48;
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
 * "A:" prefix and fopen()s the rest, so this is normally just the app's own
 * directory with a drive letter glued on front.
 *
 * A leading '/' in src is passed through as an absolute path instead
 * (A:<src>, not A:<dir>/<src>). Needed for a builtin app: its own dir is
 * always /fs/apps/<id> on flash-backed LittleFS (extracted from
 * EMBED_TXTFILES at boot, see main/builtin), but icon assets must stay
 * off flash entirely (CLAUDE.md, "Что НЕ делать") — the only place they can
 * live is the SD card, at a path with nothing to do with a builtin app's own
 * (flash) directory. A user app's own dir is already on SD, so this is a
 * no-op for it — plain relative src still resolves next to its own
 * ui.jsonl, same as before. */
static void image_apply(lv_obj_t *obj, const char *filename)
{
    if (!obj || !filename || !*filename || !s_app) {
        return;
    }
    char path[192];
    if (filename[0] == '/') {
        snprintf(path, sizeof(path), "A:%s", filename);
    } else {
        snprintf(path, sizeof(path), "A:%s/%s", s_app->dir, filename);
    }
    lv_image_set_src(obj, path);
}

/* Defined later, alongside dispatch_tap_action() it calls into; add_view()
 * below needs it as an LVGL event callback before that point in the file. */
static void view_tap_cb(lv_event_t *e);

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
    int label_w = 0;   /* declared "w" on a label, used below to center "scale"'s pivot */

    lv_obj_t *obj = NULL;

    wview_kind_t wkind = WVIEW_LABEL;

    if (strcmp(kind, "label") == 0) {
        obj = lv_label_create(s_screen);

        const cJSON *w = cJSON_GetObjectItem(line, "w");
        if (cJSON_IsNumber(w)) {
            label_w = (int)cJSON_GetNumberValue(w);
            lv_obj_set_width(obj, label_w);
            lv_label_set_long_mode(obj, LV_LABEL_LONG_WRAP);
        }

        /* Opt-in and additive: absent "align" keeps every existing widget's
         * left-aligned text exactly as it was. Only meaningful together with
         * "w" — centering text within a box the label doesn't have (no
         * explicit width) is a no-op, LVGL just hugs the text either way. */
        const char *align = cJSON_GetStringValue(cJSON_GetObjectItem(line, "align"));
        if (align && strcmp(align, "center") == 0) {
            lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
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

        const cJSON *radius = cJSON_GetObjectItem(line, "radius");
        if (cJSON_IsNumber(radius)) {
            lv_obj_set_style_radius(obj, (int)cJSON_GetNumberValue(radius), LV_PART_MAIN);
        }
        const cJSON *border_w = cJSON_GetObjectItem(line, "border_width");
        if (cJSON_IsNumber(border_w)) {
            lv_obj_set_style_border_width(obj, (int)cJSON_GetNumberValue(border_w), LV_PART_MAIN);
            lv_obj_set_style_border_color(obj,
                lv_color_hex(colour_for(cJSON_GetObjectItem(line, "border_color"), 0x445566)),
                LV_PART_MAIN);
        }
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
             * widget screen should be draggable under the resistive touch.
             * (The common tail below removes LV_OBJ_FLAG_CLICKABLE from
             * every view anyway, but an arc's clickability also gates
             * whether it is drag-adjustable, so spell it out here too.) */
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

    /* ROADMAP #39: blows up a label past whatever the largest bundled
     * Montserrat size (48) alone gives — needed for "digits filling the
     * screen" without embedding a custom bitmap font (CLAUDE.md, "не
     * вкомпилировать шрифты"). Percent, 100 = unchanged; e.g. 200 doubles
     * both axes. Pivot is the view's own (x,y) corner, not its center — the
     * enlarged render grows right/down from the position already given in
     * ui.jsonl, so placement still has to account for the bigger box by
     * hand — except horizontally when "w" is also given: the pivot then
     * defaults to the middle of that declared width (matching "align":
     * "center", which almost always accompanies it), so the enlarged text
     * grows symmetrically left/right from mid-box instead of only
     * rightward — that pairing is what actually centers a scaled label on
     * screen, since the transform itself never reflows layout. Vertically
     * the pivot stays the top edge (y unaffected by "w"); nothing here has
     * asked for vertical centering yet.
     * Only exercised so far on WVIEW_LABEL (clock.ui.jsonl); left generic
     * rather than gated by kind since the risk this is hedging against —
     * lv_image_set_scale() breaking rendering outright (see #40, image_apply)
     * — is a different LVGL code path (that one mutates the image widget's
     * own draw descriptor; this is a generic style transform any widget
     * honours), not the same bug wearing a different field name. */
    const cJSON *scale_node = cJSON_GetObjectItem(line, "scale");
    if (cJSON_IsNumber(scale_node)) {
        const int32_t scale256 = (int32_t)(cJSON_GetNumberValue(scale_node) * 256.0 / 100.0);
        lv_obj_set_style_transform_pivot_x(obj, label_w > 0 ? label_w / 2 : 0, LV_PART_MAIN);
        lv_obj_set_style_transform_pivot_y(obj, 0, LV_PART_MAIN);
        lv_obj_set_style_transform_scale_x(obj, scale256, LV_PART_MAIN);
        lv_obj_set_style_transform_scale_y(obj, scale256, LV_PART_MAIN);
    }

    /* LVGL objects default to clickable, arc's own creation branch above
     * already had to opt back out of that for the same reason: whichever
     * object is on top at a touch point eats the click outright, it is not
     * "clickable with no listener, so fall through to what's underneath".
     * A decorative rect or a label sitting inside a tappable region (see
     * "tap" below) would silently swallow taps meant for it depending on
     * exactly where the finger lands. Only "tap" opts an object back in. */
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);

    const char *cond = cJSON_GetStringValue(cJSON_GetObjectItem(line, "if"));
    if (cond) {
        s_objs[s_nobjs].cond = strdup(cond);
        /* No root exists yet, so this always resolves false: a conditioned
         * view starts hidden and is corrected on the first refresh, same as
         * a data-bound label starts as "--" until then. */
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }

    const cJSON *page_node = cJSON_GetObjectItem(line, "page");
    s_objs[s_nobjs].page = cJSON_IsNumber(page_node) ? (int)cJSON_GetNumberValue(page_node) : -1;
    if (s_objs[s_nobjs].page > s_max_page) {
        s_max_page = s_objs[s_nobjs].page;
    }
    /* Page membership is static (known right now, not data-dependent like
     * "if"), so decide visibility immediately rather than waiting for a
     * refresh that may never come (a page with no data source at all). */
    if (s_objs[s_nobjs].page >= 0 && s_objs[s_nobjs].page != s_page) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }

    const char *tap = cJSON_GetStringValue(cJSON_GetObjectItem(line, "tap"));
    if (tap) {
        s_objs[s_nobjs].tap = strdup(tap);
        lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(obj, view_tap_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)s_nobjs);

        /* A plain lv_obj gets no default-theme press feedback the way a real
         * lv_button does — found on hardware while chasing #39's format
         * toggle: with zero visual response to a touch-down, there was no
         * way to tell "I missed the target" apart from "I hit it and the
         * action is a no-op", which is exactly the ambiguity that made a
         * genuine bug (or a miss) unfalsifiable from the user's side. Any
         * touch on a tappable rect now visibly lightens it immediately,
         * before dispatch_tap_action ever runs — confirms the tap landed
         * regardless of what the action itself does or doesn't change. */
        if (wkind == WVIEW_RECT) {
            const lv_color_t base = lv_obj_get_style_bg_color(obj, LV_PART_MAIN);
            lv_obj_set_style_bg_color(obj, lv_color_lighten(base, 60),
                                       (lv_style_selector_t)LV_PART_MAIN | LV_STATE_PRESSED);
        }
    }

    s_objs[s_nobjs].obj = obj;
    s_objs[s_nobjs].kind = wkind;
    s_nobjs++;
}

/* Defined later, alongside the other refresh-time merge logic; "data" and
 * "config" lines below need them during parsing too, before that point. */
static void merge_fields(cJSON **dest, cJSON *fetched);
static void merge_source(const char *name, cJSON *fetched);

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

    /* Literal starting values, merged into the same document http/config
     * sources write into — declared once, at parse time, not refreshed.
     * Typically a widget's built-in defaults, declared before a "config"
     * line so the SD file (if any) can override them field-by-field. */
    const cJSON *data_obj = cJSON_GetObjectItem(root, "data");
    if (cJSON_IsObject(data_obj)) {
        merge_fields(&s_config, cJSON_Duplicate(data_obj, true));
        merge_source(NULL, cJSON_Duplicate(data_obj, true));
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
            /* {{...}} in the URL itself resolves against whatever "data"/
             * "config" lines already ran above this one — lets the actual
             * fetch target (city coordinates, a stock symbol, ...) come from
             * user config instead of being hardcoded per widget. A URL with
             * no {{...}} renders to itself, so existing widgets are unaffected. */
            datasource_render(url, s_combined, s_dicts, s->url, sizeof(s->url));
            const int every = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(root, "every"));
            s->every_s = (every < REFRESH_MIN_S) ? REFRESH_MIN_S : (uint32_t)every;
            s->elapsed_s = s->every_s;   /* due immediately on the scheduler's first tick */
        }
    } else if (src && strcmp(src, "config") == 0) {
        /* One-shot local read, not polled: a small JSON file (typically on
         * SD, written by webcfg) merged in exactly like "data" above, just
         * from disk instead of a literal. Missing or malformed file is not
         * an error — whatever "data" already declared (or blank fields)
         * stands, same graceful-degradation rule as every other source here. */
        const char *cfg_path = cJSON_GetStringValue(cJSON_GetObjectItem(root, "path"));
        if (!cfg_path || !*cfg_path) {
            report_error(line_no, "config source missing 'path'");
        } else {
            FILE *cf = fopen(cfg_path, "rb");
            if (cf) {
                char cfgbuf[256];
                const size_t cn = fread(cfgbuf, 1, sizeof(cfgbuf) - 1, cf);
                fclose(cf);
                cfgbuf[cn] = '\0';
                cJSON *cfg_root = cJSON_Parse(cfgbuf);
                if (cfg_root) {
                    merge_fields(&s_config, cJSON_Duplicate(cfg_root, true));
                    merge_source(NULL, cfg_root);
                }
            }
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

        if (s_objs[i].cond || s_objs[i].page >= 0) {
            const bool page_ok = s_objs[i].page < 0 || s_objs[i].page == s_page;
            const bool cond_ok = !s_objs[i].cond ||
                                  datasource_truthy(datasource_resolve(root, s_objs[i].cond));
            if (page_ok && cond_ok) {
                lv_obj_remove_flag(s_objs[i].obj, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(s_objs[i].obj, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

/* The one thing a screen tap can currently ask for. Small and fixed on
 * purpose, same reasoning as manifest "bindings" only knowing "refresh"
 * (ROADMAP #16) — grow this list when a real widget needs another action,
 * not speculatively. An unrecognised name is a silent no-op. */
static void dispatch_tap_action(const char *action)
{
    if (strcmp(action, "next_page") == 0) {
        if (s_max_page > 0) {
            s_page = (s_page + 1) % (s_max_page + 1);
            apply_data(s_combined);
            if (s_app) {
                page_nvs_save(s_app, s_page);
            }
        }
    }
}

static void view_tap_cb(lv_event_t *e)
{
    const size_t idx = (size_t)(uintptr_t)lv_event_get_user_data(e);
    if (idx < s_nobjs && s_objs[idx].tap) {
        dispatch_tap_action(s_objs[idx].tap);
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
    if (net_time_valid() && s_cache_ts > 0 && time(NULL) - s_cache_ts > WIDGET_CACHE_MAX_AGE_S) {
        ESP_LOGI(TAG, "cache too old (%lds), ignoring: %s", (long)(time(NULL) - s_cache_ts), path);
        cJSON_Delete(wrapper);
        return false;
    }
    /* Spread onto whatever is already in s_combined rather than replacing it
     * outright — same merge semantics merge_source() already uses for a live
     * fetch, a cache is just an old fetch. This can still leave a stale
     * "data"/"config" field on top of a fresher one (an old cache carries
     * whatever name/lat/lon were current when it was written) — the caller
     * reasserts s_config right after this call specifically to undo that. */
    merge_source(NULL, cJSON_Duplicate(data, true));
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

/* Spreads fetched's top-level fields onto *dest (creating it if needed),
 * overwriting same-named keys — a redeclare replaces, same convention "dict"
 * uses. Takes ownership of `fetched` either way. Factored out of
 * merge_source() so the "data"/"config" bookkeeping below can use the exact
 * same overlay semantics against a second, separate document. */
static void merge_fields(cJSON **dest, cJSON *fetched)
{
    if (!*dest) {
        *dest = cJSON_CreateObject();
    }
    cJSON *child = fetched->child;
    while (child) {
        cJSON *next = child->next;
        cJSON_DeleteItemFromObject(*dest, child->string);
        cJSON_DetachItemViaPointer(fetched, child);
        cJSON_AddItemToObject(*dest, child->string, child);
        child = next;
    }
    cJSON_Delete(fetched);   /* now an empty shell */
}

/* Folds one source's freshly-fetched body into the persistent combined
 * document, taking ownership of `fetched` either way. Unnamed sources spread
 * their fields at the top level — the exact shape a single-source widget has
 * always rendered against — so existing ui.jsonl files need no changes.
 * Named sources nest under their own key instead. */
static void merge_source(const char *name, cJSON *fetched)
{
    if (name && *name) {
        if (!s_combined) {
            s_combined = cJSON_CreateObject();
        }
        cJSON_DeleteItemFromObject(s_combined, name);
        cJSON_AddItemToObject(s_combined, name, fetched);
        return;
    }
    merge_fields(&s_combined, fetched);
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
            strftime(buf, sizeof(buf), "%I:%M", &tm_local);
            /* newlib's strftime has no "%-I" (no-leading-zero) flag, unlike
             * glibc — trimmed by hand instead for a plain single-digit hour. */
            cJSON_AddStringToObject(root, "time12", buf[0] == '0' ? buf + 1 : buf);
            strftime(buf, sizeof(buf), "%p", &tm_local);
            cJSON_AddStringToObject(root, "ampm", buf);
            cJSON_AddBoolToObject(root, "h24", s_clock_h24);
            cJSON_AddBoolToObject(root, "h12", !s_clock_h24);
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
    /* Restored before parsing, not after: add_view() below decides each
     * page-tagged view's initial hidden state from s_page at creation time,
     * so the persisted page has to already be in place or the first frame
     * would flash page 0 regardless. A stale value from a since-shrunk
     * ui.jsonl is caught below, once s_max_page is actually known. */
    s_page = page_nvs_load(app);
    s_max_page = 0;
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

    /* A persisted page from a ui.jsonl that has since shrunk (or never had
     * pages at all) would otherwise leave every page-tagged view hidden —
     * indistinguishable from a blank widget. Views were already built above
     * against the unclamped s_page; only s_page itself needs fixing up here,
     * apply_data() re-derives visibility from it on the very first refresh
     * that follows (clock/http/host all call it before anything is shown). */
    if (s_page < 0 || s_page > s_max_page) {
        s_page = 0;
    }
    for (size_t i = 0; i < s_nobjs; i++) {
        if (s_objs[i].obj && s_objs[i].page >= 0) {
            if (s_objs[i].page == s_page) {
                lv_obj_remove_flag(s_objs[i].obj, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(s_objs[i].obj, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

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
        clock_format_load();
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
            /* cache_load() just overlaid a disk cache that may well predate
             * this session's "data"/"config" lines, or belong to a since-
             * changed config (a different city, say) — reassert them on top
             * so a stale cached "name" cannot outlive a live config change.
             * The cache's own fields (current/daily, ...) are untouched:
             * this only ever re-adds the specific keys "data"/"config" gave. */
            if (s_config) {
                merge_fields(&s_combined, cJSON_Duplicate(s_config, true));
            }
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
        free(s_objs[i].tap);
        s_objs[i].tmpl = NULL;
        s_objs[i].cond = NULL;
        s_objs[i].tap = NULL;
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
    if (s_config) {
        cJSON_Delete(s_config);
        s_config = NULL;
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

void widget_refresh_now(void)
{
    if (s_nsources == 0) {
        return;   /* nothing to fetch — host topics arrive by push, not on demand */
    }
    for (size_t i = 0; i < s_nsources; i++) {
        s_sources[i].elapsed_s = s_sources[i].every_s;
    }
    /* Visible the instant the button is pressed, not only once the fetch
     * (up to ~1s away, plus network time) actually lands — otherwise a
     * refresh that was already showing "" looks like the button did nothing.
     * Called from shell_tick, which already runs on the LVGL task, so no
     * separate lvgl_port_lock() is needed here (see its own doc comment). */
    set_status("refreshing...");
}
