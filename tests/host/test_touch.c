/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 *
 * Gesture recognition from synthetic sample streams. No hardware, no clock: the
 * tests drive timestamps and coordinates explicitly, which is the point of
 * keeping the recognizer pure.
 */
#include "zv_test.h"
#include "zv_touch.h"
#include "zv_ui_draw.h"

#define MOVE_PX 20u
#define LONG_MS 500u
#define SWIPE_MS 600u

static void configure(zv_touch_t *touch)
{
    zv_touch_init(touch);
    zv_touch_configure(touch, LONG_MS, SWIPE_MS, MOVE_PX);
}

/* press, optionally a move, then release. */
static void feed(zv_touch_t *touch, uint32_t t0, uint32_t t1, int16_t x0, int16_t y0, int16_t x1, int16_t y1)
{
    ZV_CHECK(zv_touch_push(touch, t0, x0, y0, true));
    if (x1 != x0 || y1 != y0) {
        ZV_CHECK(zv_touch_push(touch, t1, x1, y1, true));
    }
    ZV_CHECK(zv_touch_push(touch, t1 + 1u, x1, y1, false));
}

static void test_no_release_is_never_a_gesture(void)
{
    zv_touch_t touch;
    configure(&touch);
    ZV_CHECK(zv_touch_push(&touch, 100u, 10, 10, true));
    ZV_CHECK_EQ(zv_touch_classify(&touch), ZV_GESTURE_NONE);
    /* Held past the threshold but never released: still nothing. */
    ZV_CHECK(zv_touch_push(&touch, 5000u, 10, 10, true));
    ZV_CHECK_EQ(zv_touch_classify(&touch), ZV_GESTURE_NONE);
    ZV_CHECK_EQ(zv_touch_held_ms(&touch, 5000u), 4900u);
}

static void test_tap(void)
{
    zv_touch_t touch;
    configure(&touch);
    feed(&touch, 100u, 140u, 50, 50, 50, 50);
    ZV_CHECK_EQ(zv_touch_classify(&touch), ZV_GESTURE_TAP);
}

static void test_jitter_is_not_a_swipe(void)
{
    zv_touch_t touch;
    configure(&touch);
    /* Travel under the 20 px floor must stay a tap, never a swipe. */
    feed(&touch, 100u, 140u, 50, 50, 50 + (int16_t)(MOVE_PX - 1u), 50);
    ZV_CHECK_EQ(zv_touch_classify(&touch), ZV_GESTURE_TAP);
}

static void test_long_press(void)
{
    zv_touch_t touch;
    configure(&touch);
    feed(&touch, 100u, 100u + LONG_MS + 50u, 50, 50, 50, 50);
    ZV_CHECK_EQ(zv_touch_classify(&touch), ZV_GESTURE_LONG_PRESS);
}

static void test_swipes(void)
{
    zv_touch_t touch;

    configure(&touch);
    feed(&touch, 100u, 160u, 50, 50, 200, 50);
    ZV_CHECK_EQ(zv_touch_classify(&touch), ZV_GESTURE_SWIPE_RIGHT);

    configure(&touch);
    feed(&touch, 100u, 160u, 200, 50, 50, 50);
    ZV_CHECK_EQ(zv_touch_classify(&touch), ZV_GESTURE_SWIPE_LEFT);

    configure(&touch);
    feed(&touch, 100u, 160u, 50, 50, 50, 200);
    ZV_CHECK_EQ(zv_touch_classify(&touch), ZV_GESTURE_SWIPE_DOWN);

    configure(&touch);
    feed(&touch, 100u, 160u, 50, 200, 50, 50);
    ZV_CHECK_EQ(zv_touch_classify(&touch), ZV_GESTURE_SWIPE_UP);
}

static void test_diagonal_picks_one_axis(void)
{
    zv_touch_t touch;
    /* Exact 45 degrees: X wins by the strict >= rule, deterministically. */
    configure(&touch);
    feed(&touch, 100u, 160u, 50, 50, 150, 150);
    ZV_CHECK_EQ(zv_touch_classify(&touch), ZV_GESTURE_SWIPE_RIGHT);

    configure(&touch);
    feed(&touch, 100u, 160u, 150, 150, 50, 50);
    ZV_CHECK_EQ(zv_touch_classify(&touch), ZV_GESTURE_SWIPE_LEFT);

    /* X just under Y must flip to the vertical axis instead of double-reporting. */
    configure(&touch);
    feed(&touch, 100u, 160u, 50, 50, 130, 170);
    ZV_CHECK_EQ(zv_touch_classify(&touch), ZV_GESTURE_SWIPE_DOWN);
}

static void test_slow_travel_is_a_drag(void)
{
    zv_touch_t touch;
    configure(&touch);
    /* Movement that exceeds the swipe window is a drag, not a flick. */
    feed(&touch, 100u, 100u + SWIPE_MS + 100u, 50, 50, 250, 50);
    ZV_CHECK_EQ(zv_touch_classify(&touch), ZV_GESTURE_DRAG);
}

static void test_ring_overflow_is_bounded(void)
{
    zv_touch_t touch;
    configure(&touch);
    /* Far more samples than the ring holds. The count must saturate and the
     * recognizer must stay in a defined state. */
    for (uint32_t i = 0u; i < ZV_TOUCH_SAMPLES * 4u; i++) {
        ZV_CHECK(zv_touch_push(&touch, 1000u + i * 10u, (int16_t)(i % 300u), (int16_t)(i % 200u), true));
    }
    ZV_CHECK_EQ(zv_touch_sample_count(&touch), ZV_TOUCH_SAMPLES);
    ZV_CHECK(zv_touch_push(&touch, 9000u, 10, 10, false));
    ZV_CHECK_EQ(zv_touch_sample_count(&touch), ZV_TOUCH_SAMPLES);
    /* Still a valid gesture value, not garbage. */
    ZV_CHECK(zv_gesture_name(zv_touch_classify(&touch)) != NULL);
}

static void test_timestamps_must_advance(void)
{
    zv_touch_t touch;
    configure(&touch);
    ZV_CHECK(zv_touch_push(&touch, 1000u, 50, 50, true));

    /* Equal timestamp refused. */
    ZV_CHECK(!zv_touch_push(&touch, 1000u, 50, 50, true));
    /* Backwards timestamp refused. */
    ZV_CHECK(!zv_touch_push(&touch, 900u, 50, 50, true));
    ZV_CHECK_EQ(zv_touch_classify(&touch), ZV_GESTURE_NONE);
    /* State was not disturbed by the refused pushes. */
    ZV_CHECK(zv_touch_push(&touch, 1001u, 50, 50, false));
    ZV_CHECK_EQ(zv_touch_classify(&touch), ZV_GESTURE_TAP);

    /* A legitimate first sample at t=0 must not open a monotonicity gap. */
    zv_touch_t zero;
    configure(&zero);
    ZV_CHECK(zv_touch_push(&zero, 0u, 50, 50, true));
    ZV_CHECK(!zv_touch_push(&zero, 0u, 50, 50, true));
    ZV_CHECK(zv_touch_push(&zero, 10u, 50, 50, false));
    ZV_CHECK_EQ(zv_touch_classify(&zero), ZV_GESTURE_TAP);
}

static void test_zero_duration_is_not_a_gesture(void)
{
    zv_touch_t touch;
    configure(&touch);
    /* Release on the same millisecond as the press: no duration, no intent. */
    ZV_CHECK(zv_touch_push(&touch, 1000u, 50, 50, true));
    /* Same-ms release is refused by the monotonic rule, so drive a real release
     * and assert the guard directly via the held-time accessor instead. */
    ZV_CHECK(!zv_touch_push(&touch, 1000u, 50, 50, false));
    ZV_CHECK_EQ(zv_touch_classify(&touch), ZV_GESTURE_NONE);
    ZV_CHECK_EQ(zv_touch_held_ms(&touch, 1000u), 0u);
}

static void test_pure_and_repeatable(void)
{
    zv_touch_t a;
    zv_touch_t b;
    configure(&a);
    configure(&b);
    for (uint32_t t = 0u; t < 20u; t++) {
        uint32_t ms = 100u + t * 15u;
        int16_t x = (int16_t)(50 + (int32_t)t * 8);
        ZV_CHECK(zv_touch_push(&a, ms, x, 50, true));
        ZV_CHECK(zv_touch_push(&b, ms, x, 50, true));
    }
    ZV_CHECK(zv_touch_push(&a, 500u, 200, 50, false));
    ZV_CHECK(zv_touch_push(&b, 500u, 200, 50, false));
    /* Identical streams, identical verdicts: no hidden state between calls. */
    ZV_CHECK_EQ(zv_touch_classify(&a), zv_touch_classify(&b));
    ZV_CHECK_EQ(zv_touch_classify(&a), ZV_GESTURE_SWIPE_RIGHT);
    ZV_CHECK_EQ(zv_touch_sample_count(&a), zv_touch_sample_count(&b));
}

static void test_reset_clears_result(void)
{
    zv_touch_t touch;
    configure(&touch);
    feed(&touch, 100u, 140u, 50, 50, 50, 50);
    ZV_CHECK_EQ(zv_touch_classify(&touch), ZV_GESTURE_TAP);
    zv_touch_reset(&touch);
    ZV_CHECK_EQ(zv_touch_classify(&touch), ZV_GESTURE_NONE);
    ZV_CHECK_EQ(zv_touch_sample_count(&touch), 0u);
    /* Thresholds survive a reset so a caller does not silently lose its tuning. */
    ZV_CHECK_EQ(touch.move_threshold_px, MOVE_PX);
}

/* ------------------------------------------------------------- menu focus */

static void test_menu_focus_is_device_derived(void)
{
    zv_touch_menu_t menu;
    zv_touch_menu_init(&menu, 4u);
    zv_touch_menu_geometry(&menu, 40, 320, 100, 30);
    menu.menu_open = true;
    ZV_CHECK_EQ(menu.item_count, 4u);

    /* Hit-testing resolves the item from local geometry only. */
    uint8_t index = 0u;
    ZV_CHECK(zv_touch_menu_hit(&menu, 100, 105, &index));
    ZV_CHECK_EQ(index, 0u);
    ZV_CHECK(zv_touch_menu_hit(&menu, 100, 135, &index));
    ZV_CHECK_EQ(index, 1u);
    ZV_CHECK(zv_touch_menu_hit(&menu, 100, 195, &index));
    ZV_CHECK_EQ(index, 3u);
    /* Outside the band, or past the last item, is not a hit. */
    ZV_CHECK(!zv_touch_menu_hit(&menu, 100, 250, &index));
    ZV_CHECK(!zv_touch_menu_hit(&menu, 10, 105, &index));

    /* Activation returns the device-derived index and updates focus. */
    menu.focus = 0u;
    uint8_t activated = 0u;
    ZV_CHECK_EQ(zv_touch_menu_activate(&menu, 100, 135, &activated), ZV_OK);
    ZV_CHECK_EQ(activated, 1u);
    ZV_CHECK_EQ(menu.focus, 1u);
    ZV_CHECK_EQ(zv_touch_menu_activate(&menu, 100, 999, &activated), ZV_ERR_INVALID_ARG);
}

static void test_menu_focus_wraps(void)
{
    zv_touch_menu_t menu;
    zv_touch_menu_init(&menu, 3u);
    ZV_CHECK_EQ(menu.focus, 0u);

    ZV_CHECK_EQ(zv_touch_menu_move_focus(&menu, ZV_GESTURE_SWIPE_DOWN), ZV_OK);
    ZV_CHECK_EQ(menu.focus, 1u);
    ZV_CHECK_EQ(zv_touch_menu_move_focus(&menu, ZV_GESTURE_SWIPE_DOWN), ZV_OK);
    ZV_CHECK_EQ(menu.focus, 2u);
    ZV_CHECK_EQ(zv_touch_menu_move_focus(&menu, ZV_GESTURE_SWIPE_DOWN), ZV_OK);
    ZV_CHECK_EQ(menu.focus, 0u); /* wraps down */
    ZV_CHECK_EQ(zv_touch_menu_move_focus(&menu, ZV_GESTURE_SWIPE_UP), ZV_OK);
    ZV_CHECK_EQ(menu.focus, 2u); /* wraps up */

    /* Horizontal swipes and non-gestures do not move focus. */
    ZV_CHECK_EQ(zv_touch_menu_move_focus(&menu, ZV_GESTURE_SWIPE_RIGHT), ZV_ERR_UNSUPPORTED);
    ZV_CHECK_EQ(menu.focus, 2u);
    ZV_CHECK_EQ(zv_touch_menu_move_focus(&menu, ZV_GESTURE_TAP), ZV_ERR_UNSUPPORTED);
    ZV_CHECK_EQ(menu.focus, 2u);

    /* Item count is clamped, and an empty menu refuses to move. */
    zv_touch_menu_t clamped;
    zv_touch_menu_init(&clamped, 9u);
    ZV_CHECK_EQ(clamped.item_count, ZV_TOUCH_MENU_MAX);
    zv_touch_menu_t empty;
    zv_touch_menu_init(&empty, 0u);
    ZV_CHECK_EQ(zv_touch_menu_move_focus(&empty, ZV_GESTURE_SWIPE_DOWN), ZV_ERR_INVALID_STATE);
}

static void test_gesture_names(void)
{
    ZV_CHECK_STR(zv_gesture_name(ZV_GESTURE_NONE), "none");
    ZV_CHECK_STR(zv_gesture_name(ZV_GESTURE_TAP), "tap");
    ZV_CHECK_STR(zv_gesture_name(ZV_GESTURE_LONG_PRESS), "long_press");
    ZV_CHECK_STR(zv_gesture_name(ZV_GESTURE_DRAG), "drag");
    ZV_CHECK_STR(zv_gesture_name(ZV_GESTURE_SWIPE_LEFT), "swipe_left");
    ZV_CHECK_STR(zv_gesture_name(ZV_GESTURE_SWIPE_RIGHT), "swipe_right");
    ZV_CHECK_STR(zv_gesture_name(ZV_GESTURE_SWIPE_UP), "swipe_up");
    ZV_CHECK_STR(zv_gesture_name(ZV_GESTURE_SWIPE_DOWN), "swipe_down");
}

static void test_release_ignores_idle_coordinates(void)
{
    zv_touch_t touch;
    configure(&touch);
    ZV_CHECK(zv_touch_push(&touch, 100u, 234, 329, true));
    ZV_CHECK(zv_touch_push(&touch, 180u, 0, 0, false));
    ZV_CHECK_EQ(zv_touch_classify(&touch), ZV_GESTURE_TAP);
    ZV_CHECK_EQ(touch.end_x, 234);
    ZV_CHECK_EQ(touch.end_y, 329);
    ZV_CHECK(zv_ui_home_hit(touch.end_x, touch.end_y));
    ZV_CHECK(zv_touch_push(&touch, 220u, 160, 150, true));
    ZV_CHECK_EQ(zv_touch_classify(&touch), ZV_GESTURE_NONE);
    ZV_CHECK(zv_touch_push(&touch, 260u, 160, 153, true));
    ZV_CHECK(zv_touch_push(&touch, 300u, 0, 0, false));
    ZV_CHECK_EQ(zv_touch_classify(&touch), ZV_GESTURE_TAP);
    uint8_t row = 0u;
    ZV_CHECK(zv_ui_menu_hit(touch.end_x, touch.end_y, 4u, &row));
    ZV_CHECK_EQ(row, 1u);
    ZV_CHECK(zv_touch_push(&touch, 400u, 50, 50, true));
    ZV_CHECK(zv_touch_push(&touch, 440u, 100, 50, true));
    ZV_CHECK(zv_touch_push(&touch, 480u, 0, 0, false));
    ZV_CHECK_EQ(zv_touch_classify(&touch), ZV_GESTURE_SWIPE_RIGHT);
}

void test_suite_touch(void)
{
    test_no_release_is_never_a_gesture();
    test_tap();
    test_release_ignores_idle_coordinates();
    test_jitter_is_not_a_swipe();
    test_long_press();
    test_swipes();
    test_diagonal_picks_one_axis();
    test_slow_travel_is_a_drag();
    test_ring_overflow_is_bounded();
    test_timestamps_must_advance();
    test_zero_duration_is_not_a_gesture();
    test_pure_and_repeatable();
    test_reset_clears_result();
    test_menu_focus_is_device_derived();
    test_menu_focus_wraps();
    test_gesture_names();
}