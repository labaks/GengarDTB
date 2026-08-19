#include "widget.h"

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
#include "lvgl.h"
#include "net.h"

static const char *TAG = "widget";

#define WIDGET_MAX_OBJS   24
#define WIDGET_MAX_UI     8192      /* a layer-A layout is a handful of lines */
#define WIDGET_TEXT_MAX   128
#define REFRESH_MIN_S     30        /* floor, so a typo cannot hammer an API   */

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

static char              s_url[256];
static uint32_t          s_every_s;
static bool              s_has_clock;
static TaskHandle_t      s_task;
static volatile bool     s_stop;

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

static void add_view(const cJSON *line)
{
    if (s_nobjs >= WIDGET_MAX_OBJS) {
        ESP_LOGW(TAG, "more than %d views, ignoring the rest", WIDGET_MAX_OBJS);
        return;
    }

    const char *kind = cJSON_GetStringValue(cJSON_GetObjectItem(line, "obj"));
    if (!kind) {
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
        ESP_LOGW(TAG, "unknown view type '%s' — skipped", kind);
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

static void parse_line(const char *line)
{
    cJSON *root = cJSON_Parse(line);
    if (!root) {
        ESP_LOGW(TAG, "skipping malformed line");
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
        if (url) {
            snprintf(s_url, sizeof(s_url), "%s", url);
            const int every = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(root, "every"));
            s_every_s = (every < REFRESH_MIN_S) ? REFRESH_MIN_S : (uint32_t)every;
        }
    } else if (src && strcmp(src, "clock") == 0) {
        /* Local source: no fetch, no network dependency. Ticks once a second
         * so a minute rollover shows up promptly instead of waiting out the
         * http floor above. */
        s_has_clock = true;
    } else {
        add_view(root);
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

static void refresh_task(void *arg)
{
    (void)arg;

    while (!s_stop) {
        if (net_state() != NET_UP) {
            if (lvgl_port_lock(200)) {
                set_status("waiting for network");
                lvgl_port_unlock();
            }
        } else {
            cJSON *root = NULL;
            const esp_err_t err = datasource_fetch_json(s_url, &root);

            if (lvgl_port_lock(500)) {
                if (err == ESP_OK) {
                    apply_data(root);
                    set_status("");
                } else {
                    /* Keep the last values on screen and say they are stale.
                     * Blanking the widget on a transient network hiccup is
                     * strictly worse than showing slightly old numbers. */
                    set_status("stale");
                }
                lvgl_port_unlock();
            }
            if (root) {
                cJSON_Delete(root);
            }
        }

        /* Sleep in slices so closing the widget does not wait a whole period. */
        for (uint32_t i = 0; i < s_every_s * 5 && !s_stop; i++) {
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }

    s_task = NULL;
    vTaskDelete(NULL);
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
    s_url[0] = '\0';
    s_every_s = 0;
    s_has_clock = false;
    memset(s_objs, 0, sizeof(s_objs));

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

    char *save = NULL;
    for (char *line = strtok_r(buf, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
        while (*line == ' ' || *line == '\t') {
            line++;
        }
        if (*line == '\0' || *line == '#') {
            continue;
        }
        parse_line(line);
    }
    free(buf);

    lv_screen_load(s_screen);
    ESP_LOGI(TAG, "opened '%s': %u view(s)%s", app->id, (unsigned)s_nobjs,
             s_has_clock ? ", clock source" : (s_url[0] ? ", http source" : ", static"));

    /* Clock and http are alternatives, not layered — ROADMAP #10 (multiple sources
     * per widget) is not built yet. A manifest declaring both gets the clock,
     * since that is the one that does not depend on the network being up. */
    if (s_has_clock) {
        s_stop = false;
        if (xTaskCreatePinnedToCore(clock_task, "wdg_clock", 4096, NULL, 3, &s_task, 1)
                != pdPASS) {
            ESP_LOGE(TAG, "cannot start clock task");
            s_task = NULL;
        }
    } else if (s_url[0]) {
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
