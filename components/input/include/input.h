/*
 * deskos input layer — buttons, chords and bindings.
 *
 * The shell and widgets never read a GPIO. They consume (mask, kind) events, which
 * means the same code runs against real buttons, on-screen touch zones, or events
 * injected from the PC — see input_backend_t.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Three buttons is all the board has free GPIOs for. 7 non-empty masks x 2 kinds
 * = 14 bindable actions, which is plenty. */
#define INPUT_B1        (1u << 0)
#define INPUT_B2        (1u << 1)
#define INPUT_B3        (1u << 2)
#define INPUT_MASK_ALL  (INPUT_B1 | INPUT_B2 | INPUT_B3)

typedef enum {
    INPUT_EV_CLICK,         /* short press of `mask`, fired on release */
    INPUT_EV_DOUBLE_CLICK,  /* second click within INPUT_DOUBLE_MS — fired IN ADDITION
                             * to the first CLICK, never instead of it (see input.c) */
    INPUT_EV_LONG_PRESS,    /* held past INPUT_LONG_MS, fired once while still held */
    INPUT_EV_HOLD_REPEAT,   /* autorepeat after a long press, for value adjustment */
    INPUT_EV_RELEASE,       /* released after a long press */
} input_ev_kind_t;

typedef struct {
    uint8_t          mask;  /* INPUT_B* bitmask, e.g. INPUT_B1|INPUT_B2 for a chord */
    input_ev_kind_t  kind;
} input_event_t;

typedef enum {
    /* Real buttons on GPIO22/27/35. Requires soldering. */
    INPUT_BACKEND_GPIO,

    /* Three zones across the bottom of the screen. The panel is RESISTIVE, so this
     * backend is SINGLE-TOUCH: it can never produce a chord. Fine for navigation,
     * useless for testing chord bindings. */
    INPUT_BACKEND_TOUCH_ZONES,

    /* Synthetic events pushed in via input_inject(). This is how chords get tested
     * before any buttons exist, since it can assert any mask. */
    INPUT_BACKEND_DEBUG_INJECT,
} input_backend_t;

/* Timing. The chord window is the load-bearing one: without it, pressing B1+B2
 * always decodes as B1 followed by B2, because no two fingers land on the same
 * millisecond. 50 ms of latency is invisible in menu navigation. */
#define INPUT_POLL_MS      5
#define INPUT_DEBOUNCE_MS  10
#define INPUT_CHORD_MS     50
#define INPUT_LONG_MS      500
#define INPUT_REPEAT_MS    250
#define INPUT_DOUBLE_MS    300

/* Which buttons physically exist. Call BEFORE input_init.
 *
 * This is not cosmetic: GPIO35 has no internal pull-up, so if it is declared
 * present but not wired with its external 10k it floats and the device presses
 * B3 for itself. Pins outside the mask are never configured or read. */
void input_set_present_mask(uint8_t mask);
uint8_t input_get_present_mask(void);

esp_err_t input_init(input_backend_t backend);

/* Switch backends at runtime — useful for flipping to touch zones when the
 * calibration app runs, or to debug injection from a serial command. */
esp_err_t input_set_backend(input_backend_t backend);
input_backend_t input_get_backend(void);

/* Pops one event. timeout_ms == 0 polls, UINT32_MAX blocks forever. */
bool input_get_event(input_event_t *out, uint32_t timeout_ms);

/* Assert/release a raw mask for INPUT_BACKEND_DEBUG_INJECT. The normal state
 * machine still runs on top, so injected input produces real chords, long
 * presses and autorepeat exactly as physical buttons would. */
void input_inject(uint8_t mask);

/* "B1+B3 long" — for logs and for the bindings editor UI. Not reentrant. */
const char *input_event_str(const input_event_t *ev);

/* Inverse of input_event_str() — for reading a manifest's "bindings" object
 * keys (see docs/app-format.md). Accepts exactly what input_event_str()
 * produces: "B1", "B1+B2" or "B1+B2+B3" (any button order), a space, then one
 * of click/dblclick/long/repeat/release. Returns false without touching
 * *out on anything else — a typo in a manifest must not silently bind to
 * mask 0. */
bool input_parse_event_str(const char *s, input_event_t *out);

#ifdef __cplusplus
}
#endif
