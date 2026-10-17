#pragma once
#include <stdbool.h>
#include <stdint.h>

enum { DOT_BUTTON_CENTRE = 1, DOT_BUTTON_UP = 2, DOT_BUTTON_DOWN = 4 };
enum { DOT_PRESS_CENTRE = 1, DOT_PRESS_UP = 2, DOT_PRESS_DOWN = 4,
       DOT_HOLD_CENTRE = 8, DOT_HOLD_SIDES = 16 };
typedef struct {
    uint32_t changed_at[3], pressed_at[3], repeated_at[3], chord_at;
    uint8_t raw, stable, repeated;
    bool centre_held, chord, chord_fired;
} dot_buttons_t;
/* Call every 10–20 ms. Side holds repeat after 450 ms, then every >=90 ms.
 * Taps emit on release; repeats suppress that final tap. A simultaneous side
 * chord suppresses individual events until both sides are released. Unsigned
 * elapsed-time checks remain valid across timer wrap. */
unsigned dot_buttons_poll(dot_buttons_t *buttons, uint8_t raw, uint32_t now_ms);
