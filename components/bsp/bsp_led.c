#include "bsp.h"

#include "driver/gpio.h"

static bool s_inited;

/* The CYD's RGB LED is wired active-low, so every level is inverted here and
 * callers get to think in plain logic. */
void bsp_led_set(bool r, bool g, bool b)
{
    if (!s_inited) {
        const gpio_config_t cfg = {
            .pin_bit_mask = (1ULL << BSP_PIN_LED_R) |
                            (1ULL << BSP_PIN_LED_G) |
                            (1ULL << BSP_PIN_LED_B),
            .mode         = GPIO_MODE_OUTPUT,
        };
        gpio_config(&cfg);
        s_inited = true;
    }

    gpio_set_level(BSP_PIN_LED_R, !r);
    gpio_set_level(BSP_PIN_LED_G, !g);
    gpio_set_level(BSP_PIN_LED_B, !b);
}
