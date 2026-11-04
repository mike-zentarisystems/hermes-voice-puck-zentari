/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 */
#include "zv_avatar_model.h"
#include "zv_test.h"

static void test_expression_table(void)
{
    for (int s = 0; s < (int)ZV_AVATAR_STATE_COUNT; s++) {
        const zv_expression_t *e = zv_avatar_expression((zv_avatar_state_t)s);
        ZV_CHECK(e != NULL);
        if (e != NULL) {
            ZV_CHECK(e->eye_open <= 1000u);
            ZV_CHECK(e->ring_intensity <= 1000u);
        }
    }
    ZV_CHECK(zv_avatar_expression(ZV_AVATAR_STATE_COUNT) == NULL);
    ZV_CHECK_EQ(zv_avatar_expression(ZV_AVATAR_IDLE)->ring_rgb, ZV_COLOR_ZENTARI_BLUE);
    ZV_CHECK_EQ(zv_avatar_expression(ZV_AVATAR_LISTENING)->ring_rgb, ZV_COLOR_ZENTARI_BLUE);
    ZV_CHECK_EQ(zv_avatar_expression(ZV_AVATAR_SUCCESS)->ring_rgb, ZV_COLOR_SUCCESS);
    ZV_CHECK_EQ(zv_avatar_expression(ZV_AVATAR_ERROR)->ring_rgb, ZV_COLOR_ERROR);
    ZV_CHECK_EQ(zv_avatar_expression(ZV_AVATAR_APPROVAL)->ring_rgb, ZV_COLOR_APPROVAL);
    ZV_CHECK(zv_avatar_expression(ZV_AVATAR_SPEAKING)->lipsync_enabled);
    ZV_CHECK(!zv_avatar_expression(ZV_AVATAR_APPROVAL)->blink_enabled);
}

static void test_blinking(void)
{
    zv_avatar_model_t model;
    ZV_CHECK_EQ(zv_avatar_init(&model, 0, 1234u), ZV_OK);
    zv_face_frame_t frame;
    int blinks = 0;
    bool was_closed = false;
    for (uint32_t t = 0; t <= 20000u; t += 10u) {
        ZV_CHECK_EQ(zv_avatar_frame(&model, t, NULL, 0, &frame), ZV_OK);
        bool closed = frame.eye_open < 200u;
        if (closed && !was_closed) {
            blinks++;
        }
        was_closed = closed;
    }
    /* Interval is 2.5 s .. 6 s, so 20 s yields between 3 and 8 blinks. */
    ZV_CHECK(blinks >= 3);
    ZV_CHECK(blinks <= 8);

    ZV_CHECK_EQ(zv_avatar_set_state(&model, ZV_AVATAR_APPROVAL, 20000u), ZV_OK);
    for (uint32_t t = 20000u; t <= 40000u; t += 10u) {
        ZV_CHECK_EQ(zv_avatar_frame(&model, t, NULL, 0, &frame), ZV_OK);
        ZV_CHECK_EQ(frame.eye_open, 1000);
    }
}

static void test_state_effects(void)
{
    zv_avatar_model_t model;
    ZV_CHECK_EQ(zv_avatar_init(&model, 0, 7u), ZV_OK);
    ZV_CHECK_EQ(zv_avatar_set_state(&model, ZV_AVATAR_SPEAKING, 100u), ZV_OK);
    ZV_CHECK_EQ(zv_avatar_set_state(&model, ZV_AVATAR_SPEAKING, 900u), ZV_OK);
    ZV_CHECK_EQ(model.state_entered_ms, 100u); /* unchanged state keeps its clock */

    zv_mouth_shape_t mouth = {.open = 640u, .width = 520u, .round = 300u};
    zv_face_frame_t frame;
    ZV_CHECK_EQ(zv_avatar_frame(&model, 1000u, &mouth, 950u, &frame), ZV_OK);
    ZV_CHECK_EQ(frame.mouth_open, 640);
    ZV_CHECK_EQ(frame.mouth_round, 300);
    ZV_CHECK_EQ(frame.ring_intensity, 950);
    ZV_CHECK_EQ(zv_avatar_frame(&model, 1010u, &mouth, 10u, &frame), ZV_OK);
    ZV_CHECK_EQ(frame.ring_intensity, 400); /* floor = half of base intensity */

    ZV_CHECK_EQ(zv_avatar_set_state(&model, ZV_AVATAR_THINKING, 2000u), ZV_OK);
    for (uint32_t t = 2000u; t < 8000u; t += 37u) {
        ZV_CHECK_EQ(zv_avatar_frame(&model, t, &mouth, 0u, &frame), ZV_OK);
        ZV_CHECK(frame.ring_sweep);
        ZV_CHECK(frame.ring_sweep_deg < 360u);
        ZV_CHECK_EQ(frame.mouth_open, zv_avatar_expression(ZV_AVATAR_THINKING)->mouth_open); /* no lip-sync */
    }

    ZV_CHECK_EQ(zv_avatar_set_state(&model, ZV_AVATAR_STATE_COUNT, 0u), ZV_ERR_INVALID_ARG);
    ZV_CHECK_EQ(zv_avatar_frame(NULL, 0u, NULL, 0u, &frame), ZV_ERR_INVALID_ARG);
    ZV_CHECK_EQ(zv_avatar_init(NULL, 0u, 0u), ZV_ERR_INVALID_ARG);
}

void test_suite_avatar_model(void)
{
    test_expression_table();
    test_blinking();
    test_state_effects();
}
