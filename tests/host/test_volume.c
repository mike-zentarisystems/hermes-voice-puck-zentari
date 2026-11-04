/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 */
#include "zv_test.h"
#include "zv_volume.h"

static void test_init_and_defaults(void)
{
    zv_volume_t v;
    zv_volume_init(&v, 40);
    ZV_CHECK_EQ(zv_volume_level(&v), 40);
    ZV_CHECK(!zv_volume_is_muted(&v));
    ZV_CHECK_EQ(zv_volume_effective(&v), 40);

    /* Out-of-range defaults clamp rather than wrap. */
    zv_volume_init(&v, 250);
    ZV_CHECK_EQ(zv_volume_level(&v), ZV_VOLUME_MAX);

    /* NULL is survivable everywhere: the shell never passes one, but a null
     * deref here would take the panel down instead of failing a check. */
    ZV_CHECK_EQ(zv_volume_level(NULL), 0);
    ZV_CHECK(!zv_volume_is_muted(NULL));
    ZV_CHECK_EQ(zv_volume_effective(NULL), 0);
    ZV_CHECK_EQ(zv_volume_bars(NULL), 0);
    ZV_CHECK_EQ(zv_volume_set_level(NULL, 10), ZV_ERR_INVALID_ARG);
    ZV_CHECK_EQ(zv_volume_step(NULL, 1), ZV_ERR_INVALID_ARG);
    ZV_CHECK_EQ(zv_volume_set_muted(NULL, true), ZV_ERR_INVALID_ARG);
    ZV_CHECK(!zv_volume_toggle_mute(NULL));
}

/*
 * The reason mute is a flag and not a level: unmuting has to give back the
 * volume the operator chose. A design that stored silence as level 0 passes a
 * "mute silences" test and still fails this one.
 */
static void test_mute_remembers_level(void)
{
    zv_volume_t v;
    zv_volume_init(&v, 65);

    ZV_CHECK(zv_volume_toggle_mute(&v));
    ZV_CHECK(zv_volume_is_muted(&v));
    ZV_CHECK_EQ(zv_volume_effective(&v), 0); /* the amplifier really does go silent */
    ZV_CHECK_EQ(zv_volume_level(&v), 65);    /* but the choice is still there */
    ZV_CHECK_EQ(zv_volume_bars(&v), 0);

    ZV_CHECK(!zv_volume_toggle_mute(&v));
    ZV_CHECK(!zv_volume_is_muted(&v));
    ZV_CHECK_EQ(zv_volume_effective(&v), 65);
    ZV_CHECK_EQ(zv_volume_level(&v), 65);

    /* Changing the level while muted is allowed and takes effect on unmute,
     * rather than being silently discarded. */
    ZV_CHECK(zv_volume_set_muted(&v, true) == ZV_OK);
    ZV_CHECK_EQ(zv_volume_set_level(&v, 20), ZV_OK);
    ZV_CHECK_EQ(zv_volume_effective(&v), 0);
    ZV_CHECK(zv_volume_set_muted(&v, false) == ZV_OK);
    ZV_CHECK_EQ(zv_volume_effective(&v), 20);
}

static void test_set_and_step_clamp(void)
{
    zv_volume_t v;
    zv_volume_init(&v, 50);

    ZV_CHECK_EQ(zv_volume_set_level(&v, 255), ZV_OK);
    ZV_CHECK_EQ(zv_volume_level(&v), ZV_VOLUME_MAX);
    ZV_CHECK_EQ(zv_volume_set_level(&v, 0), ZV_OK);
    ZV_CHECK_EQ(zv_volume_level(&v), 0);

    zv_volume_init(&v, 50);
    ZV_CHECK_EQ(zv_volume_step(&v, (int8_t)ZV_VOLUME_STEP), ZV_OK);
    ZV_CHECK_EQ(zv_volume_level(&v), 60);
    ZV_CHECK_EQ(zv_volume_step(&v, (int8_t)ZV_VOLUME_STEP), ZV_OK);
    ZV_CHECK_EQ(zv_volume_level(&v), 70);

    /* Saturating, not wrapping: 95 + 10 is 100, not 5. */
    zv_volume_init(&v, 95);
    ZV_CHECK_EQ(zv_volume_step(&v, (int8_t)ZV_VOLUME_STEP), ZV_OK);
    ZV_CHECK_EQ(zv_volume_level(&v), ZV_VOLUME_MAX);

    /* And the same downwards, where wrapping would jump to full volume. */
    zv_volume_init(&v, 5);
    ZV_CHECK_EQ(zv_volume_step(&v, -(int8_t)ZV_VOLUME_STEP), ZV_OK);
    ZV_CHECK_EQ(zv_volume_level(&v), 0);

    zv_volume_init(&v, 50);
    ZV_CHECK_EQ(zv_volume_step(&v, 0), ZV_OK);
    ZV_CHECK_EQ(zv_volume_level(&v), 50);
}

static void test_indicator_bars(void)
{
    zv_volume_t v;

    zv_volume_init(&v, 0);
    ZV_CHECK_EQ(zv_volume_bars(&v), 0);
    zv_volume_init(&v, ZV_VOLUME_MAX);
    ZV_CHECK_EQ(zv_volume_bars(&v), ZV_VOLUME_BARS);

    /* Half volume is half the segments, not zero and not everything. */
    zv_volume_init(&v, 50);
    ZV_CHECK_EQ(zv_volume_bars(&v), ZV_VOLUME_BARS / 2u);

    /* Rounds to nearest so a just-over-half level does not read as empty. */
    zv_volume_init(&v, 55);
    ZV_CHECK_EQ(zv_volume_bars(&v), 6);

    zv_volume_init(&v, 100);
    ZV_CHECK_EQ(zv_volume_bars(&v), ZV_VOLUME_BARS);
}

static void test_mute_names(void)
{
    ZV_CHECK_STR(zv_mute_state_name(true), "muted");
    ZV_CHECK_STR(zv_mute_state_name(false), "unmuted");
}

/*
 * The cadence that keeps the clock honest. The default is one minute; what is
 * actually asserted is the property the clock depends on, that a frame is never
 * more than one period stale and that the opening frame always paints.
 */
static void test_repaint_cadence(void)
{
    zv_repaint_t r;

    zv_repaint_init(&r, 60000u);
    ZV_CHECK_EQ(zv_repaint_period_ms(&r), 60000u);

    /* Nothing primed yet: the first frame paints immediately. */
    ZV_CHECK(zv_repaint_due(&r, 0u));

    zv_repaint_mark(&r, 1000u);
    ZV_CHECK(!zv_repaint_due(&r, 1000u));
    ZV_CHECK(!zv_repaint_due(&r, 1000u + 59999u));
    ZV_CHECK(zv_repaint_due(&r, 61000u));
    ZV_CHECK(zv_repaint_due(&r, 61001u));

    zv_repaint_mark(&r, 61000u);
    ZV_CHECK(!zv_repaint_due(&r, 120000u));

    /* A clock that wraps mid-interval reads as elapsed time. Compared in signed
     * arithmetic this would be a large negative delta and read as "not due"
     * until roughly 24 days had passed. Here 61440 ms elapse across the wrap. */
    zv_repaint_t w;
    zv_repaint_init(&w, 60000u);
    zv_repaint_mark(&w, 0xFFFF0000u);
    ZV_CHECK(!zv_repaint_due(&w, 0xFFFF0000u + 30000u)); /* 30 s in, still under */
    ZV_CHECK(zv_repaint_due(&w, 0x0000F000u));           /* 61440 ms in, across the wrap */

    /* A period below the floor is clamped rather than honoured, so a stray 0
     * cannot turn the shell into a redraw loop. */
    zv_repaint_init(&w, 0u);
    ZV_CHECK_EQ(zv_repaint_period_ms(&w), ZV_REPAINT_MIN_PERIOD_MS);
    zv_repaint_init(&w, 500u);
    ZV_CHECK_EQ(zv_repaint_period_ms(&w), ZV_REPAINT_MIN_PERIOD_MS);

    ZV_CHECK(!zv_repaint_due(NULL, 0u));
    zv_repaint_mark(NULL, 0u);
    ZV_CHECK_EQ(zv_repaint_period_ms(NULL), 0u);
}

void test_suite_volume(void)
{
    test_init_and_defaults();
    test_mute_remembers_level();
    test_set_and_step_clamp();
    test_indicator_bars();
    test_mute_names();
    test_repaint_cadence();
}