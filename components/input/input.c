#include "input.h"

#include <stdio.h>
#include <string.h>

#include "bsp.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "input";

static QueueHandle_t   s_queue;
static input_backend_t s_backend;
static volatile uint8_t s_injected;     /* written by input_inject(), read by the task */

/* ------------------------------------------------------------------ backends */

static uint8_t s_present = INPUT_MASK_ALL;

void input_set_present_mask(uint8_t mask)
{
    s_present = mask & INPUT_MASK_ALL;
}

uint8_t input_get_present_mask(void)
{
    return s_present;
}

static uint8_t read_gpio(void)
{
    uint8_t m = 0;
    /* Active low: pressed pulls the pin to GND. B1/B2 use internal pull-ups,
     * B3 (GPIO35) is input-only with no internal pull-up and depends on the
     * external 10k to 3V3 — without that resistor it floats and reads garbage,
     * which is why unwired buttons are skipped entirely rather than read. */
    if ((s_present & INPUT_B1) && gpio_get_level(BSP_PIN_BTN1) == 0) { m |= INPUT_B1; }
    if ((s_present & INPUT_B2) && gpio_get_level(BSP_PIN_BTN2) == 0) { m |= INPUT_B2; }
    if ((s_present & INPUT_B3) && gpio_get_level(BSP_PIN_BTN3) == 0) { m |= INPUT_B3; }
    return m;
}

static uint8_t read_touch_zones(void)
{
    bsp_touch_state_t t = { 0 };
    bsp_touch_read(&t);

    /* Bottom strip only, so the rest of the screen stays usable by widgets. */
    if (!t.pressed || t.y < (BSP_LCD_V_RES - 48)) {
        return 0;
    }

    /* Single-touch panel: exactly one bit, ever. */
    if (t.x < BSP_LCD_H_RES / 3)     { return INPUT_B1; }
    if (t.x < 2 * BSP_LCD_H_RES / 3) { return INPUT_B2; }
    return INPUT_B3;
}

static uint8_t read_raw(void)
{
    switch (s_backend) {
    case INPUT_BACKEND_GPIO:         return read_gpio();
    case INPUT_BACKEND_TOUCH_ZONES:  return read_touch_zones();
    case INPUT_BACKEND_DEBUG_INJECT: return s_injected;
    default:                         return 0;
    }
}

/* ------------------------------------------------------- chord state machine */

typedef enum {
    ST_IDLE,     /* nothing held */
    ST_COLLECT,  /* something is down; still waiting to see if more buttons join */
    ST_HELD,     /* mask is settled */
} state_t;

static void emit(uint8_t mask, input_ev_kind_t kind)
{
    const input_event_t ev = { .mask = mask, .kind = kind };
    if (xQueueSend(s_queue, &ev, 0) != pdTRUE) {
        ESP_LOGW(TAG, "event queue full, dropped %s", input_event_str(&ev));
    }
}

static void input_task(void *arg)
{
    (void)arg;

    state_t  state         = ST_IDLE;
    uint8_t  stable        = 0;      /* debounced raw mask */
    uint8_t  candidate     = 0;      /* raw mask awaiting debounce confirmation */
    int64_t  candidate_at  = 0;

    uint8_t  chord         = 0;      /* the settled chord being reported on */
    int64_t  chord_started = 0;
    int64_t  next_repeat   = 0;
    bool     long_fired    = false;

    uint8_t  last_click    = 0;      /* for double-click detection */
    int64_t  last_click_at = 0;

    for (;;) {
        const int64_t now = esp_timer_get_time() / 1000;   /* ms */
        const uint8_t raw = read_raw();

        /* Debounce: a change must persist for INPUT_DEBOUNCE_MS to count. */
        if (raw != candidate) {
            candidate = raw;
            candidate_at = now;
        } else if (raw != stable && (now - candidate_at) >= INPUT_DEBOUNCE_MS) {
            stable = raw;
        }

        switch (state) {
        case ST_IDLE:
            if (stable != 0) {
                chord = stable;
                chord_started = now;
                state = ST_COLLECT;
            }
            break;

        case ST_COLLECT:
            /* Accumulate rather than overwrite: fingers never land simultaneously,
             * and a chord must survive the first button being sampled alone. */
            chord |= stable;

            if (stable == 0) {
                /* Released inside the chord window — a very fast tap. Report it. */
                goto released;
            }
            if ((now - chord_started) >= INPUT_CHORD_MS) {
                long_fired = false;
                next_repeat = 0;
                state = ST_HELD;
            }
            break;

        case ST_HELD:
            if (stable == 0) {
                goto released;
            }
            if (!long_fired && (now - chord_started) >= INPUT_LONG_MS) {
                emit(chord, INPUT_EV_LONG_PRESS);
                long_fired = true;
                next_repeat = now + INPUT_REPEAT_MS;
            } else if (long_fired && now >= next_repeat) {
                emit(chord, INPUT_EV_HOLD_REPEAT);
                next_repeat = now + INPUT_REPEAT_MS;
            }
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(INPUT_POLL_MS));
        continue;

    released:
        if (long_fired) {
            emit(chord, INPUT_EV_RELEASE);
        } else {
            /* CLICK fires immediately and DOUBLE_CLICK is an extra event on top of
             * it, rather than CLICK being delayed by INPUT_DOUBLE_MS to find out.
             * Menu navigation must not pay 300 ms of latency for a feature most
             * widgets ignore; anyone who cares about double-click handles both. */
            emit(chord, INPUT_EV_CLICK);

            if (chord == last_click && (now - last_click_at) <= INPUT_DOUBLE_MS) {
                emit(chord, INPUT_EV_DOUBLE_CLICK);
                last_click = 0;          /* a triple tap is not two double-clicks */
                last_click_at = 0;
            } else {
                last_click = chord;
                last_click_at = now;
            }
        }

        chord = 0;
        long_fired = false;
        state = ST_IDLE;
        vTaskDelay(pdMS_TO_TICKS(INPUT_POLL_MS));
    }
}

/* --------------------------------------------------------------------- setup */

static esp_err_t gpio_backend_init(void)
{
    uint64_t pulled_pins = 0;
    if (s_present & INPUT_B1) { pulled_pins |= 1ULL << BSP_PIN_BTN1; }
    if (s_present & INPUT_B2) { pulled_pins |= 1ULL << BSP_PIN_BTN2; }

    if (pulled_pins) {
        const gpio_config_t pulled = {
            .pin_bit_mask = pulled_pins,
            .mode         = GPIO_MODE_INPUT,
            .pull_up_en   = GPIO_PULLUP_ENABLE,
        };
        const esp_err_t err = gpio_config(&pulled);
        if (err != ESP_OK) {
            return err;
        }
    }

    /* GPIO35 accepts no internal pull-up; the external resistor is mandatory,
     * so it is only configured when declared present. */
    if (s_present & INPUT_B3) {
        const gpio_config_t bare = {
            .pin_bit_mask = (1ULL << BSP_PIN_BTN3),
            .mode         = GPIO_MODE_INPUT,
        };
        return gpio_config(&bare);
    }

    ESP_LOGI(TAG, "buttons wired: %s%s%s",
             (s_present & INPUT_B1) ? "B1 " : "",
             (s_present & INPUT_B2) ? "B2 " : "",
             (s_present & INPUT_B3) ? "B3" : "");
    return ESP_OK;
}

esp_err_t input_set_backend(input_backend_t backend)
{
    if (backend == INPUT_BACKEND_GPIO) {
        const esp_err_t err = gpio_backend_init();
        if (err != ESP_OK) {
            return err;
        }
    }
    s_injected = 0;
    s_backend = backend;
    ESP_LOGI(TAG, "backend = %d", (int)backend);
    return ESP_OK;
}

input_backend_t input_get_backend(void)
{
    return s_backend;
}

esp_err_t input_init(input_backend_t backend)
{
    s_queue = xQueueCreate(16, sizeof(input_event_t));
    if (!s_queue) {
        return ESP_ERR_NO_MEM;
    }

    const esp_err_t err = input_set_backend(backend);
    if (err != ESP_OK) {
        return err;
    }

    /* Core 0: the UI lives on core 1 and must not be perturbed by a 5 ms poller. */
    if (xTaskCreatePinnedToCore(input_task, "input", 3072, NULL, 6, NULL, 0) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool input_get_event(input_event_t *out, uint32_t timeout_ms)
{
    if (!out || !s_queue) {
        return false;
    }
    const TickType_t ticks = (timeout_ms == UINT32_MAX) ? portMAX_DELAY
                                                        : pdMS_TO_TICKS(timeout_ms);
    return xQueueReceive(s_queue, out, ticks) == pdTRUE;
}

void input_inject(uint8_t mask)
{
    s_injected = mask & INPUT_MASK_ALL;
}

bool input_parse_event_str(const char *s, input_event_t *out)
{
    if (!s || !out) {
        return false;
    }
    const char *sp = strchr(s, ' ');
    if (!sp) {
        return false;
    }

    char maskpart[16];
    const size_t masklen = (size_t)(sp - s);
    if (masklen == 0 || masklen >= sizeof(maskpart)) {
        return false;
    }
    memcpy(maskpart, s, masklen);
    maskpart[masklen] = '\0';

    uint8_t mask = 0;
    char *save = NULL;
    for (char *tok = strtok_r(maskpart, "+", &save); tok; tok = strtok_r(NULL, "+", &save)) {
        if (strcmp(tok, "B1") == 0) {
            mask |= INPUT_B1;
        } else if (strcmp(tok, "B2") == 0) {
            mask |= INPUT_B2;
        } else if (strcmp(tok, "B3") == 0) {
            mask |= INPUT_B3;
        } else {
            return false;
        }
    }
    if (mask == 0) {
        return false;
    }

    static const struct {
        const char      *name;
        input_ev_kind_t  kind;
    } kinds[] = {
        { "click",    INPUT_EV_CLICK },
        { "dblclick", INPUT_EV_DOUBLE_CLICK },
        { "long",     INPUT_EV_LONG_PRESS },
        { "repeat",   INPUT_EV_HOLD_REPEAT },
        { "release",  INPUT_EV_RELEASE },
    };
    const char *kindstr = sp + 1;
    for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
        if (strcmp(kindstr, kinds[i].name) == 0) {
            out->mask = mask;
            out->kind = kinds[i].kind;
            return true;
        }
    }
    return false;
}

const char *input_event_str(const input_event_t *ev)
{
    static char buf[32];
    if (!ev) {
        return "(null)";
    }

    char m[12] = { 0 };
    size_t n = 0;
    if (ev->mask & INPUT_B1) { m[n++] = 'B'; m[n++] = '1'; }
    if (ev->mask & INPUT_B2) { if (n) { m[n++] = '+'; } m[n++] = 'B'; m[n++] = '2'; }
    if (ev->mask & INPUT_B3) { if (n) { m[n++] = '+'; } m[n++] = 'B'; m[n++] = '3'; }
    if (!n) { m[n++] = '-'; }

    static const char *kinds[] = { "click", "dblclick", "long", "repeat", "release" };
    snprintf(buf, sizeof(buf), "%s %s", m, kinds[ev->kind]);
    return buf;
}
