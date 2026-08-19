/*
 * Pure logic: resolve a dotted path against a cJSON document, run it through
 * an optional filter pipeline, and substitute {{...}} placeholders in a
 * template string. No I/O, no ESP-IDF headers — this is what tests/datasource
 * builds and runs on the host. The impure HTTP fetch lives in datasource_http.c.
 */
#include "datasource.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

const cJSON *datasource_resolve(const cJSON *root, const char *path)
{
    if (!root || !path || !*path) {
        return NULL;
    }

    const cJSON *node = root;
    const char *p = path;

    while (*p && node) {
        char key[48];
        size_t n = 0;
        while (*p && *p != '.' && n < sizeof(key) - 1) {
            key[n++] = *p++;
        }
        key[n] = '\0';
        if (*p == '.') {
            p++;
        }
        if (n == 0) {
            return NULL;
        }

        /* An all-digits component indexes an array; anything else is a key. */
        bool numeric = true;
        for (size_t i = 0; i < n; i++) {
            if (key[i] < '0' || key[i] > '9') {
                numeric = false;
                break;
            }
        }

        if (numeric && cJSON_IsArray(node)) {
            node = cJSON_GetArrayItem(node, atoi(key));
        } else {
            node = cJSON_GetObjectItemCaseSensitive(node, key);
        }
    }

    return node;
}

bool datasource_truthy(const cJSON *node)
{
    if (!node) {
        return false;
    }
    if (cJSON_IsBool(node)) {
        return cJSON_IsTrue(node);
    }
    if (cJSON_IsNumber(node)) {
        return node->valuedouble != 0;
    }
    if (cJSON_IsString(node)) {
        const char *s = node->valuestring;
        return *s != '\0' && strcmp(s, "0") != 0 && strcmp(s, "false") != 0;
    }
    if (cJSON_IsArray(node) || cJSON_IsObject(node)) {
        return cJSON_GetArraySize(node) > 0;
    }
    return true;   /* present, and none of the false-ish shapes above */
}

/* ------------------------------------------------------------ filter value */

/* A value in flight through the filter pipeline. Starts life as whatever
 * datasource_resolve found; filters transform it in place. decimals is -1
 * ("auto": integers print bare, everything else keeps one decimal) until a
 * "round" filter pins it to an explicit precision. */
typedef enum { DV_MISSING, DV_NUMBER, DV_STRING, DV_BOOL } dvalue_kind_t;

#define DVALUE_TEXT_MAX 64   /* any buffer fed by dvalue_to_text() must be at
                               * least this big, or gcc flags a possible
                               * -Werror=format-truncation on the %s copy */

typedef struct {
    dvalue_kind_t kind;
    double        number;
    int           decimals;
    char          text[DVALUE_TEXT_MAX];
} dvalue_t;

static dvalue_t dvalue_from_node(const cJSON *node)
{
    dvalue_t v = { .kind = DV_MISSING, .decimals = -1 };

    if (!node) {
        /* stays DV_MISSING */
    } else if (cJSON_IsNumber(node)) {
        v.kind = DV_NUMBER;
        v.number = node->valuedouble;
    } else if (cJSON_IsString(node)) {
        v.kind = DV_STRING;
        snprintf(v.text, sizeof(v.text), "%s", node->valuestring);
    } else if (cJSON_IsBool(node)) {
        v.kind = DV_BOOL;
        v.number = cJSON_IsTrue(node) ? 1 : 0;
    }
    return v;
}

static void dvalue_to_text(const dvalue_t *v, char *tmp, size_t tmp_size)
{
    switch (v->kind) {
    case DV_STRING:
        snprintf(tmp, tmp_size, "%s", v->text);
        break;
    case DV_NUMBER:
        if (v->decimals >= 0) {
            snprintf(tmp, tmp_size, "%.*f", v->decimals, v->number);
        } else if (v->number == (double)(long long)v->number) {
            /* Integers should not render as "3.0"; everything else keeps one
             * decimal, which is the useful precision for temperatures and
             * rates when no explicit "round" filter said otherwise. */
            snprintf(tmp, tmp_size, "%lld", (long long)v->number);
        } else {
            snprintf(tmp, tmp_size, "%.1f", v->number);
        }
        break;
    case DV_BOOL:
        snprintf(tmp, tmp_size, "%s", v->number != 0 ? "yes" : "no");
        break;
    default:
        snprintf(tmp, tmp_size, "--");
        break;
    }
}

/* ----------------------------------------------------------------- filters */

static double round_to(double v, int decimals)
{
    double scale = 1.0;
    for (int i = 0; i < decimals; i++) {
        scale *= 10.0;
    }
    const double scaled = v * scale;
    const double rounded = (scaled >= 0) ? (double)(long long)(scaled + 0.5)
                                          : (double)(long long)(scaled - 0.5);
    return rounded / scale;
}

static void filter_round(dvalue_t *v, const char *arg)
{
    if (v->kind != DV_NUMBER) {
        return;
    }
    int decimals = (arg && *arg) ? atoi(arg) : 0;
    if (decimals < 0) {
        decimals = 0;
    } else if (decimals > 6) {
        decimals = 6;
    }
    v->number = round_to(v->number, decimals);
    v->decimals = decimals;
}

/* arg is a literal number ("0.621") or a path to resolve against root
 * ("current.temperature_2m") — whichever it parses as. */
static bool filter_operand(const char *arg, const cJSON *root, double *out)
{
    if (!arg || !*arg) {
        return false;
    }
    char *end = NULL;
    const double literal = strtod(arg, &end);
    if (end != arg && *end == '\0') {
        *out = literal;
        return true;
    }
    const cJSON *node = datasource_resolve(root, arg);
    if (node && cJSON_IsNumber(node)) {
        *out = node->valuedouble;
        return true;
    }
    return false;
}

static void filter_arith(dvalue_t *v, const char *arg, const cJSON *root, char op)
{
    if (v->kind != DV_NUMBER) {
        return;
    }
    double operand;
    if (!filter_operand(arg, root, &operand)) {
        return;
    }
    switch (op) {
    case '+': v->number += operand; break;
    case '-': v->number -= operand; break;
    case '*': v->number *= operand; break;
    case '/': if (operand != 0) { v->number /= operand; } break;
    }
}

static void filter_dict(dvalue_t *v, const char *arg, const cJSON *dicts)
{
    if (!arg || !*arg || !dicts) {
        return;
    }
    const cJSON *map = cJSON_GetObjectItemCaseSensitive(dicts, arg);
    if (!map) {
        return;
    }

    char key[DVALUE_TEXT_MAX];
    dvalue_to_text(v, key, sizeof(key));

    const cJSON *text = cJSON_GetObjectItemCaseSensitive(map, key);
    if (text && cJSON_IsString(text)) {
        v->kind = DV_STRING;
        v->decimals = -1;
        snprintf(v->text, sizeof(v->text), "%s", text->valuestring);
    }
    /* Unknown key: leave the value as it was, so an unmapped code still
     * shows something instead of vanishing behind a "--". */
}

static void filter_date(dvalue_t *v, const char *arg)
{
    if (v->kind != DV_STRING || !arg || !*arg) {
        return;
    }

    struct tm tm = { 0 };
    int y = 0, mo = 0, d = 0, h = 0, mi = 0;
    int n = sscanf(v->text, "%d-%d-%dT%d:%d", &y, &mo, &d, &h, &mi);
    if (n < 3) {
        n = sscanf(v->text, "%d-%d-%d", &y, &mo, &d);
        if (n < 3) {
            return;   /* not a shape we recognise: leave it untouched */
        }
    }
    tm.tm_year = y - 1900;
    tm.tm_mon  = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min  = mi;

    char buf[32];
    if (strftime(buf, sizeof(buf), arg, &tm) == 0) {
        return;
    }
    v->kind = DV_STRING;
    v->decimals = -1;
    snprintf(v->text, sizeof(v->text), "%s", buf);
}

static void apply_filter(dvalue_t *v, const char *name, const char *arg,
                         const cJSON *root, const cJSON *dicts)
{
    if (strcmp(name, "round") == 0) {
        filter_round(v, arg);
    } else if (strcmp(name, "add") == 0) {
        filter_arith(v, arg, root, '+');
    } else if (strcmp(name, "sub") == 0) {
        filter_arith(v, arg, root, '-');
    } else if (strcmp(name, "mul") == 0) {
        filter_arith(v, arg, root, '*');
    } else if (strcmp(name, "div") == 0) {
        filter_arith(v, arg, root, '/');
    } else if (strcmp(name, "dict") == 0) {
        filter_dict(v, arg, dicts);
    } else if (strcmp(name, "date") == 0) {
        filter_date(v, arg);
    }
    /* An unknown filter name is a no-op: a typo should not blank the field. */
}

static void trim(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) {
        s[--n] = '\0';
    }
    char *start = s;
    while (*start == ' ' || *start == '\t') {
        start++;
    }
    if (start != s) {
        memmove(s, start, strlen(start) + 1);
    }
}

static void append_value(const dvalue_t *v, char *out, size_t out_size, size_t *used)
{
    char tmp[DVALUE_TEXT_MAX];
    dvalue_to_text(v, tmp, sizeof(tmp));

    for (const char *s = tmp; *s && *used < out_size - 1; s++) {
        out[(*used)++] = *s;
    }
}

/* {{ path | filter:arg | filter:arg ... }} — up to this many pipeline stages
 * (path plus filters); anything past that in one placeholder is ignored
 * rather than overflowing a fixed-size stack array. */
#define DATASOURCE_MAX_SEGMENTS 5

void datasource_render(const char *tmpl, const cJSON *root, const cJSON *dicts,
                       char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }
    if (!tmpl) {
        out[0] = '\0';
        return;
    }

    size_t used = 0;
    const char *p = tmpl;

    while (*p && used < out_size - 1) {
        if (p[0] == '{' && p[1] == '{') {
            const char *end = strstr(p + 2, "}}");
            if (end) {
                char content[96];
                const size_t n = (size_t)(end - (p + 2));
                if (n < sizeof(content)) {
                    memcpy(content, p + 2, n);
                    content[n] = '\0';

                    char *segs[DATASOURCE_MAX_SEGMENTS];
                    int nsegs = 0;
                    segs[nsegs++] = content;
                    for (char *c = content; *c; c++) {
                        if (*c == '|' && nsegs < DATASOURCE_MAX_SEGMENTS) {
                            *c = '\0';
                            segs[nsegs++] = c + 1;
                        }
                    }
                    for (int i = 0; i < nsegs; i++) {
                        trim(segs[i]);
                    }

                    dvalue_t value = dvalue_from_node(datasource_resolve(root, segs[0]));
                    for (int i = 1; i < nsegs; i++) {
                        char *colon = strchr(segs[i], ':');
                        const char *arg = NULL;
                        if (colon) {
                            *colon = '\0';
                            arg = colon + 1;
                        }
                        apply_filter(&value, segs[i], arg, root, dicts);
                    }

                    append_value(&value, out, out_size, &used);
                }
                p = end + 2;
                continue;
            }
        }
        out[used++] = *p++;
    }

    out[used] = '\0';
}
