#include "widget.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

typedef struct {
    lv_obj_t *obj;
    char     *tmpl;                 /* NULL when the text is static */
} wobj_t;

static const app_info_t *s_app;
static lv_obj_t         *s_screen;
static lv_obj_t         *s_prev_screen;
static lv_obj_t         *s_status_label;
static wobj_t            s_objs[WIDGET_MAX_OBJS];
static size_t            s_nobjs;

static char              s_url[256];
static uint32_t          s_every_s;
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
            datasource_render(text, NULL, rendered, sizeof(rendered));
            lv_label_set_text(obj, rendered);
        } else {
            lv_label_set_text(obj, "");
        }
    } else if (strcmp(kind, "rect") == 0) {
        obj = lv_obj_create(s_screen);
        lv_obj_set_size(obj,
            (int)cJSON_GetNumberValue(cJSON_GetObjectItem(line, "w")),
            (int)cJSON_GetNumberValue(cJSON_GetObjectItem(line, "h")));
        lv_obj_set_style_bg_color(obj,
            lv_color_hex(colour_for(cJSON_GetObjectItem(line, "color"), 0x202830)),
            LV_PART_MAIN);
        lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    } else {
        ESP_LOGW(TAG, "unknown view type '%s' — skipped", kind);
        return;
    }

    lv_obj_set_pos(obj, x, y);
    s_objs[s_nobjs].obj = obj;
    s_nobjs++;
}

static void parse_line(const char *line)
{
    cJSON *root = cJSON_Parse(line);
    if (!root) {
        ESP_LOGW(TAG, "skipping malformed line");
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
        if (!s_objs[i].tmpl || !s_objs[i].obj) {
            continue;
        }
        datasource_render(s_objs[i].tmpl, root, rendered, sizeof(rendered));
        lv_label_set_text(s_objs[i].obj, rendered);
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
             s_url[0] ? ", http source" : ", static");

    if (s_url[0]) {
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
        s_objs[i].tmpl = NULL;
    }
    s_nobjs = 0;

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
