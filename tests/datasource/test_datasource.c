/* Host-side tests for datasource_resolve/datasource_render/datasource_truthy.
 * See CMakeLists.txt and ROADMAP.md #2/#4 for why this exists as a
 * separate host build. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "datasource.h"

static int g_failures = 0;

#define CHECK(cond, ...) do { \
        if (!(cond)) { \
            g_failures++; \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__); \
            fprintf(stderr, "\n"); \
        } \
    } while (0)

static void test_resolve_dotted_path(void)
{
    cJSON *root = cJSON_Parse("{\"current\":{\"temperature_2m\":21.0}}");
    const cJSON *node = datasource_resolve(root, "current.temperature_2m");
    CHECK(node && cJSON_IsNumber(node) && node->valuedouble == 21.0,
          "expected current.temperature_2m to resolve to 21.0");
    cJSON_Delete(root);
}

static void test_resolve_array_index(void)
{
    cJSON *root = cJSON_Parse("{\"daily\":{\"temperature_2m_max\":[19.5,20.1]}}");

    const cJSON *first = datasource_resolve(root, "daily.temperature_2m_max.0");
    CHECK(first && first->valuedouble == 19.5, "index 0 should resolve to 19.5");

    const cJSON *second = datasource_resolve(root, "daily.temperature_2m_max.1");
    CHECK(second && second->valuedouble == 20.1, "index 1 should resolve to 20.1");

    const cJSON *oob = datasource_resolve(root, "daily.temperature_2m_max.5");
    CHECK(oob == NULL, "out-of-range index should resolve to NULL");

    cJSON_Delete(root);
}

static void test_resolve_missing_field(void)
{
    cJSON *root = cJSON_Parse("{\"current\":{\"temperature_2m\":21.0}}");

    CHECK(datasource_resolve(root, "current.missing") == NULL,
          "missing key must resolve to NULL");
    CHECK(datasource_resolve(root, "not.even.close") == NULL,
          "an unrelated path must resolve to NULL, not crash");
    CHECK(datasource_resolve(NULL, "a.b") == NULL,
          "a NULL root must resolve to NULL");
    CHECK(datasource_resolve(root, "") == NULL,
          "an empty path must resolve to NULL");

    cJSON_Delete(root);
}

static void test_render_missing_field_shows_dashes(void)
{
    cJSON *root = cJSON_Parse("{}");
    char out[32];

    datasource_render("temp: {{current.temperature_2m}} C", root, NULL, out, sizeof(out));
    CHECK(strcmp(out, "temp: -- C") == 0,
          "a missing field must render as --, got '%s'", out);

    cJSON_Delete(root);
}

static void test_render_number_and_bool_formatting(void)
{
    cJSON *root = cJSON_Parse("{\"a\":21.0,\"b\":19.5,\"c\":true,\"d\":false}");
    char out[64];

    datasource_render("{{a}}", root, NULL, out, sizeof(out));
    CHECK(strcmp(out, "21") == 0,
          "an integral double must render without decimals, got '%s'", out);

    datasource_render("{{b}}", root, NULL, out, sizeof(out));
    CHECK(strcmp(out, "19.5") == 0,
          "a fractional double must keep one decimal, got '%s'", out);

    datasource_render("{{c}} {{d}}", root, NULL, out, sizeof(out));
    CHECK(strcmp(out, "yes no") == 0,
          "booleans must render as yes/no, got '%s'", out);

    cJSON_Delete(root);
}

static void test_render_buffer_overflow_safety(void)
{
    cJSON *root = cJSON_Parse(
        "{\"s\":\"a string that is much longer than the output buffer we give it\"}");

    const size_t out_size = 8;
    char *buf = malloc(out_size + 1);
    buf[out_size] = (char)0xAA;   /* canary just past the declared buffer */

    datasource_render("{{s}}", root, NULL, buf, out_size);

    CHECK((unsigned char)buf[out_size] == 0xAA,
          "datasource_render wrote past the out_size it was given");
    CHECK(strcmp(buf, "a strin") == 0,
          "truncated output should fill out_size - 1 and terminate, got '%s'", buf);

    free(buf);
    cJSON_Delete(root);
}

static void test_render_resolved_value_not_rescanned(void)
{
    /* Only the template is scanned for {{...}}; a resolved value that happens
     * to contain "{{...}}" text must come through verbatim, not be treated
     * as a nested placeholder. */
    cJSON *root = cJSON_Parse("{\"weird\":\"{{current.temperature_2m}}\"}");
    char out[64];

    datasource_render("[{{weird}}]", root, NULL, out, sizeof(out));
    CHECK(strcmp(out, "[{{current.temperature_2m}}]") == 0,
          "a resolved value must not be re-scanned for placeholders, got '%s'", out);

    cJSON_Delete(root);
}

static void test_render_unterminated_placeholder_is_literal(void)
{
    cJSON *root = cJSON_Parse("{}");
    char out[32];

    datasource_render("a{{b", root, NULL, out, sizeof(out));
    CHECK(strcmp(out, "a{{b") == 0,
          "an unterminated {{ with no closing }} must pass through literally, got '%s'", out);

    cJSON_Delete(root);
}

/* ------------------------------------------------------ ROADMAP #4 filters */

static void test_filter_round(void)
{
    cJSON *root = cJSON_Parse("{\"t\":19.456}");
    char out[32];

    datasource_render("{{t | round:0}}", root, NULL, out, sizeof(out));
    CHECK(strcmp(out, "19") == 0, "round:0 should drop to an integer, got '%s'", out);

    datasource_render("{{t | round:1}}", root, NULL, out, sizeof(out));
    CHECK(strcmp(out, "19.5") == 0, "round:1 should round to one decimal, got '%s'", out);

    datasource_render("{{t | round:2}}", root, NULL, out, sizeof(out));
    CHECK(strcmp(out, "19.46") == 0, "round:2 should round to two decimals, got '%s'", out);

    cJSON *str_root = cJSON_Parse("{\"s\":\"text\"}");
    datasource_render("{{s | round:1}}", str_root, NULL, out, sizeof(out));
    CHECK(strcmp(out, "text") == 0,
          "round on a non-numeric value must be a no-op, got '%s'", out);

    cJSON_Delete(root);
    cJSON_Delete(str_root);
}

static void test_filter_arithmetic_constant(void)
{
    cJSON *root = cJSON_Parse("{\"kmh\":100.0}");
    char out[32];

    /* This is what a unit conversion reads like: km/h -> mph. */
    datasource_render("{{kmh | mul:0.621 | round:0}}", root, NULL, out, sizeof(out));
    CHECK(strcmp(out, "62") == 0, "100 km/h * 0.621 rounded should be 62, got '%s'", out);

    datasource_render("{{kmh | div:0}}", root, NULL, out, sizeof(out));
    CHECK(strcmp(out, "100") == 0,
          "dividing by a literal zero must be a no-op, not crash, got '%s'", out);

    cJSON_Delete(root);
}

static void test_filter_arithmetic_path_operand(void)
{
    cJSON *root = cJSON_Parse("{\"a\":{\"temperature_2m\":21.0,\"apparent_temperature\":19.0}}");
    char out[32];

    datasource_render("{{a.temperature_2m | sub:a.apparent_temperature}}", root, NULL,
                      out, sizeof(out));
    CHECK(strcmp(out, "2") == 0,
          "sub with a path operand should compute 21 - 19, got '%s'", out);

    datasource_render("{{a.temperature_2m | add:a.missing}}", root, NULL, out, sizeof(out));
    CHECK(strcmp(out, "21") == 0,
          "an unresolvable path operand must leave the value untouched, got '%s'", out);

    cJSON_Delete(root);
}

static void test_filter_dict(void)
{
    cJSON *root = cJSON_Parse("{\"code\":3}");
    cJSON *dicts = cJSON_Parse(
        "{\"weather_code\":{\"0\":\"Clear\",\"3\":\"Cloudy\"}}");
    char out[32];

    datasource_render("{{code | dict:weather_code}}", root, dicts, out, sizeof(out));
    CHECK(strcmp(out, "Cloudy") == 0,
          "code 3 through the weather_code dict should read Cloudy, got '%s'", out);

    /* An unmapped key falls back to the plain value instead of vanishing. */
    cJSON *other_root = cJSON_Parse("{\"code\":99}");
    datasource_render("{{code | dict:weather_code}}", other_root, dicts, out, sizeof(out));
    CHECK(strcmp(out, "99") == 0,
          "an unmapped code should fall back to the raw value, got '%s'", out);

    /* An unknown dict name is a no-op, same reasoning. */
    datasource_render("{{code | dict:no_such_dict}}", root, dicts, out, sizeof(out));
    CHECK(strcmp(out, "3") == 0,
          "an unknown dict name must be a no-op, got '%s'", out);

    cJSON_Delete(root);
    cJSON_Delete(other_root);
    cJSON_Delete(dicts);
}

static void test_filter_date(void)
{
    cJSON *root = cJSON_Parse("{\"d\":\"2026-08-19\",\"dt\":\"2026-08-19T14:30\"}");
    char out[32];

    datasource_render("{{d | date:%d %b}}", root, NULL, out, sizeof(out));
    CHECK(strcmp(out, "19 Aug") == 0, "date reformat should read 19 Aug, got '%s'", out);

    datasource_render("{{dt | date:%H:%M}}", root, NULL, out, sizeof(out));
    CHECK(strcmp(out, "14:30") == 0, "date reformat should read 14:30, got '%s'", out);

    datasource_render("{{d | date:garbage}}", root, NULL, out, sizeof(out));
    CHECK(strcmp(out, "garbage") == 0,
          "a format with no % tokens just prints literally, got '%s'", out);

    cJSON *bad_root = cJSON_Parse("{\"d\":\"not a date\"}");
    datasource_render("{{d | date:%d}}", bad_root, NULL, out, sizeof(out));
    CHECK(strcmp(out, "not a date") == 0,
          "an unparsable date string must be a no-op, got '%s'", out);

    /* 2026-08-19 is a Wednesday — tm_wday used to be left at its {0}
     * initializer (Sunday) forever, so %a/%A silently lied for every date
     * other than an actual Sunday. docs/app-format.md documented this as a
     * known limitation; ROADMAP #40's weekly forecast needed it to actually
     * work, so it got fixed instead of worked around. */
    datasource_render("{{d | date:%a}}", root, NULL, out, sizeof(out));
    CHECK(strcmp(out, "Wed") == 0, "2026-08-19 is a Wednesday, got '%s'", out);

    cJSON_Delete(root);
    cJSON_Delete(bad_root);
}

static void test_filter_unknown_name_is_noop(void)
{
    cJSON *root = cJSON_Parse("{\"t\":21}");
    char out[32];

    datasource_render("{{t | frobnicate:9}}", root, NULL, out, sizeof(out));
    CHECK(strcmp(out, "21") == 0,
          "an unknown filter name must not blank the field, got '%s'", out);

    cJSON_Delete(root);
}

static void test_truthy(void)
{
    cJSON *root = cJSON_Parse(
        "{\"t\":true,\"f\":false,\"n0\":0,\"n1\":5,\"s_empty\":\"\",\"s_zero\":\"0\","
        "\"s_false\":\"false\",\"s_text\":\"yes\",\"arr_empty\":[],\"arr_full\":[1],\"nul\":null}");

    CHECK(datasource_truthy(NULL) == false, "missing must be falsy");
    CHECK(datasource_truthy(cJSON_GetObjectItem(root, "t")) == true, "true must be truthy");
    CHECK(datasource_truthy(cJSON_GetObjectItem(root, "f")) == false, "false must be falsy");
    CHECK(datasource_truthy(cJSON_GetObjectItem(root, "n0")) == false, "0 must be falsy");
    CHECK(datasource_truthy(cJSON_GetObjectItem(root, "n1")) == true, "5 must be truthy");
    CHECK(datasource_truthy(cJSON_GetObjectItem(root, "s_empty")) == false, "\"\" must be falsy");
    CHECK(datasource_truthy(cJSON_GetObjectItem(root, "s_zero")) == false, "\"0\" must be falsy");
    CHECK(datasource_truthy(cJSON_GetObjectItem(root, "s_false")) == false,
          "\"false\" must be falsy");
    CHECK(datasource_truthy(cJSON_GetObjectItem(root, "s_text")) == true, "\"yes\" must be truthy");
    CHECK(datasource_truthy(cJSON_GetObjectItem(root, "arr_empty")) == false,
          "an empty array must be falsy");
    CHECK(datasource_truthy(cJSON_GetObjectItem(root, "arr_full")) == true,
          "a non-empty array must be truthy");
    CHECK(datasource_truthy(cJSON_GetObjectItem(root, "nul")) == false,
          "JSON null must be falsy, same bucket as a missing field");

    cJSON_Delete(root);
}

int main(void)
{
    test_resolve_dotted_path();
    test_resolve_array_index();
    test_resolve_missing_field();
    test_render_missing_field_shows_dashes();
    test_render_number_and_bool_formatting();
    test_render_buffer_overflow_safety();
    test_render_resolved_value_not_rescanned();
    test_render_unterminated_placeholder_is_literal();
    test_filter_round();
    test_filter_arithmetic_constant();
    test_filter_arithmetic_path_operand();
    test_filter_dict();
    test_filter_date();
    test_filter_unknown_name_is_noop();
    test_truthy();

    if (g_failures) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("all datasource checks passed\n");
    return 0;
}
