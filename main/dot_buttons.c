#include <stdbool.h>
#include "dot_buttons.h"

unsigned dot_buttons_poll(dot_buttons_t *b, uint8_t raw, uint32_t now)
{
    unsigned actions = 0;
    for (unsigned i = 0; i < 3; ++i) {
        unsigned bit = 1U << i;
        if ((raw ^ b->raw) & bit) {
            b->raw = (b->raw & ~bit) | (raw & bit);
            b->changed_at[i] = now;
        }
        if (((b->stable ^ b->raw) & bit) && (uint32_t)(now - b->changed_at[i]) >= 40) {
            b->stable = (b->stable & ~bit) | (b->raw & bit);
            if (b->stable & bit) {
                b->pressed_at[i] = now;
                if (i == 0) b->centre_held = false;
                else b->repeated &= ~bit;
            } else if (i == 0) {
                if (!b->centre_held) actions |= DOT_PRESS_CENTRE;
            } else {
                if (!b->chord && !(b->repeated & bit)) actions |= bit;
                b->repeated &= ~bit;
            }
        }
    }
    if ((b->stable & 6) == 6 && !b->chord) {
        b->chord = true; b->chord_fired = false; b->chord_at = now;
    }
    if (b->chord && (b->stable & 6) == 6 && !b->chord_fired &&
        (uint32_t)(now - b->chord_at) >= 1000) {
        actions |= DOT_HOLD_SIDES; b->chord_fired = true;
    }
    if (b->chord && !(b->stable & 6)) b->chord = false;
    // Stop a volume repeat as soon as both raw contacts are down, including
    // the debounce interval before the chord becomes stable.
    for (unsigned i = 1; i < 3 && !b->chord && (b->raw & 6) != 6; ++i) {
        unsigned bit = 1U << i;
        if (!(b->stable & bit)) continue;
        bool repeating = (b->repeated & bit) != 0;
        if ((!repeating && (uint32_t)(now - b->pressed_at[i]) >= 450) ||
            (repeating && (uint32_t)(now - b->repeated_at[i]) >= 90)) {
            actions |= bit;
            b->repeated |= bit;
            b->repeated_at[i] = now;
        }
    }
    if ((b->stable & 1) && !b->centre_held && (uint32_t)(now - b->pressed_at[0]) >= 900) {
        actions |= DOT_HOLD_CENTRE; b->centre_held = true;
    }
    return actions;
}
