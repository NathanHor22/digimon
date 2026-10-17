/* Native tests execute the real, platform-independent production decoder. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "../main/dot_buttons.h"

static void debounce_and_taps(void) {
    dot_buttons_t b = {0};
    assert(!dot_buttons_poll(&b, 0, 0));
    assert(!dot_buttons_poll(&b, DOT_BUTTON_UP, 10));
    assert(!dot_buttons_poll(&b, 0, 25)); // A 15 ms contact bounce is ignored.
    assert(!dot_buttons_poll(&b, 0, 70));
    assert(!b.stable);
    assert(!dot_buttons_poll(&b, DOT_BUTTON_UP, 100));
    assert(!dot_buttons_poll(&b, DOT_BUTTON_UP, 139));
    assert(!dot_buttons_poll(&b, DOT_BUTTON_UP, 140));
    assert(b.stable == DOT_BUTTON_UP);
    assert(!dot_buttons_poll(&b, 0, 200));
    assert(!dot_buttons_poll(&b, 0, 239));
    assert(dot_buttons_poll(&b, 0, 240) == DOT_PRESS_UP);
    assert(!dot_buttons_poll(&b, 0, 300));
    assert(!dot_buttons_poll(&b, DOT_BUTTON_DOWN, 400));
    assert(!dot_buttons_poll(&b, DOT_BUTTON_DOWN, 440));
    assert(!dot_buttons_poll(&b, 0, 500));
    assert(dot_buttons_poll(&b, 0, 540) == DOT_PRESS_DOWN);
}

static void side_hold_repeats(void) {
    dot_buttons_t b = {0};
    assert(!dot_buttons_poll(&b, DOT_BUTTON_UP, 100));
    assert(!dot_buttons_poll(&b, DOT_BUTTON_UP, 140));
    assert(!dot_buttons_poll(&b, DOT_BUTTON_UP, 589));
    assert(dot_buttons_poll(&b, DOT_BUTTON_UP, 590) == DOT_PRESS_UP);
    assert(!dot_buttons_poll(&b, DOT_BUTTON_UP, 679));
    assert(dot_buttons_poll(&b, DOT_BUTTON_UP, 680) == DOT_PRESS_UP);
    assert(dot_buttons_poll(&b, DOT_BUTTON_UP, 770) == DOT_PRESS_UP);
    assert(!dot_buttons_poll(&b, 0, 780));
    assert(!dot_buttons_poll(&b, 0, 820)); // No extra release tap after repeats.
    // A later tap still works after the repeat state has been cleared.
    assert(!dot_buttons_poll(&b, DOT_BUTTON_UP, 900));
    assert(!dot_buttons_poll(&b, DOT_BUTTON_UP, 940));
    assert(!dot_buttons_poll(&b, 0, 980));
    assert(dot_buttons_poll(&b, 0, 1020) == DOT_PRESS_UP);
}

static void centre_hold_once(void) {
    dot_buttons_t b = {0};
    assert(!dot_buttons_poll(&b, DOT_BUTTON_CENTRE, 100));
    assert(!dot_buttons_poll(&b, DOT_BUTTON_CENTRE, 140));
    assert(!dot_buttons_poll(&b, DOT_BUTTON_CENTRE, 1039));
    assert(dot_buttons_poll(&b, DOT_BUTTON_CENTRE, 1040) == DOT_HOLD_CENTRE);
    assert(!dot_buttons_poll(&b, DOT_BUTTON_CENTRE, 5000));
    assert(!dot_buttons_poll(&b, 0, 5010));
    assert(!dot_buttons_poll(&b, 0, 5050));
    assert(!dot_buttons_poll(&b, DOT_BUTTON_CENTRE, 6000));
    assert(!dot_buttons_poll(&b, DOT_BUTTON_CENTRE, 6040));
    assert(!dot_buttons_poll(&b, 0, 6100));
    assert(dot_buttons_poll(&b, 0, 6140) == DOT_PRESS_CENTRE);
}

static void chord_suppresses_individuals(void) {
    dot_buttons_t b = {0};
    const unsigned both = DOT_BUTTON_UP | DOT_BUTTON_DOWN;
    assert(!dot_buttons_poll(&b, both, 100));
    assert(!dot_buttons_poll(&b, both, 140));
    assert(!dot_buttons_poll(&b, both, 590)); // No side repeat at 450 ms.
    assert(!dot_buttons_poll(&b, both, 1139));
    assert(dot_buttons_poll(&b, both, 1140) == DOT_HOLD_SIDES);
    assert(!dot_buttons_poll(&b, both, 3000));
    assert(!dot_buttons_poll(&b, DOT_BUTTON_UP, 3010));
    assert(!dot_buttons_poll(&b, DOT_BUTTON_UP, 3050));
    assert(!dot_buttons_poll(&b, DOT_BUTTON_UP, 4000)); // Suppressed until both up.
    assert(!dot_buttons_poll(&b, 0, 4010));
    assert(!dot_buttons_poll(&b, 0, 4050));
    assert(!b.chord);
    // Even a short chord must not accidentally change volume on release.
    assert(!dot_buttons_poll(&b, both, 5000));
    assert(!dot_buttons_poll(&b, both, 5040));
    assert(!dot_buttons_poll(&b, 0, 5100));
    assert(!dot_buttons_poll(&b, 0, 5140));
    // Adding the second side during an existing repeat suppresses the next
    // repeat immediately, even while that second contact is debouncing.
    b = (dot_buttons_t){0};
    dot_buttons_poll(&b, DOT_BUTTON_UP, 6000);
    dot_buttons_poll(&b, DOT_BUTTON_UP, 6040);
    assert(dot_buttons_poll(&b, DOT_BUTTON_UP, 6490) == DOT_PRESS_UP);
    assert(!dot_buttons_poll(&b, both, 6580));
    assert(!dot_buttons_poll(&b, both, 6600));
    assert(!dot_buttons_poll(&b, both, 6620));
    assert(dot_buttons_poll(&b, both, 7620) == DOT_HOLD_SIDES);
}

static void timer_wrap_and_polling(void) {
    dot_buttons_t b = {0};
    uint32_t start = UINT32_MAX - 100;
    assert(!dot_buttons_poll(&b, DOT_BUTTON_DOWN, start));
    assert(!dot_buttons_poll(&b, DOT_BUTTON_DOWN, start + 40));
    assert(!dot_buttons_poll(&b, DOT_BUTTON_DOWN, start + 489));
    assert(dot_buttons_poll(&b, DOT_BUTTON_DOWN, start + 490) == DOT_PRESS_DOWN);
    assert(dot_buttons_poll(&b, DOT_BUTTON_DOWN, start + 580) == DOT_PRESS_DOWN);
    assert(!dot_buttons_poll(&b, 0, start + 590));
    assert(!dot_buttons_poll(&b, 0, start + 630));
    b = (dot_buttons_t){0};
    assert(!dot_buttons_poll(&b, DOT_BUTTON_CENTRE, start));
    assert(!dot_buttons_poll(&b, DOT_BUTTON_CENTRE, start + 40));
    assert(dot_buttons_poll(&b, DOT_BUTTON_CENTRE, start + 940) == DOT_HOLD_CENTRE);
    // A slow poll emits one repeat action, never a catch-up burst.
    b = (dot_buttons_t){0};
    dot_buttons_poll(&b, DOT_BUTTON_UP, 100);
    dot_buttons_poll(&b, DOT_BUTTON_UP, 140);
    assert(dot_buttons_poll(&b, DOT_BUTTON_UP, 20000) == DOT_PRESS_UP);
    assert(!dot_buttons_poll(&b, DOT_BUTTON_UP, 20001));
}

int main(void) {
    debounce_and_taps(); side_hold_repeats(); centre_hold_once();
    chord_suppresses_individuals(); timer_wrap_and_polling();
    puts("button tests passed: debounce, taps, 450/90 ms side repeats, no release duplicate, centre hold, chord suppression, timer wrap, slow polling");
    return 0;
}
