/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 */
#include "zv_lipsync.h"
#include "zv_test.h"

#define FRAME_SAMPLES 320u /* 20 ms at 16 kHz */

static void fill_square(int16_t *buf, size_t n, int16_t amplitude)
{
    for (size_t i = 0; i < n; i++) {
        buf[i] = (i % 20u) < 10u ? amplitude : (int16_t)-amplitude;
    }
}

static void test_envelope(void)
{
    zv_envelope_t env;
    zv_envelope_config_t cfg = zv_envelope_default_config();
    ZV_CHECK_EQ(zv_envelope_init(&env, &cfg), ZV_OK);

    int16_t frame[FRAME_SAMPLES];
    uint16_t level = 1234;
    fill_square(frame, FRAME_SAMPLES, 0);
    ZV_CHECK_EQ(zv_envelope_process_pcm16(&env, frame, FRAME_SAMPLES, &level), ZV_OK);
    ZV_CHECK_EQ(level, 0);

    fill_square(frame, FRAME_SAMPLES, 16000);
    uint16_t previous = 0;
    for (int i = 0; i < 30; i++) {
        ZV_CHECK_EQ(zv_envelope_process_pcm16(&env, frame, FRAME_SAMPLES, &level), ZV_OK);
        ZV_CHECK(level >= previous);
        previous = level;
    }
    ZV_CHECK_EQ(level, 1000);

    fill_square(frame, FRAME_SAMPLES, 0);
    int frames_to_close = 0;
    while (level > 0 && frames_to_close < 500) {
        ZV_CHECK_EQ(zv_envelope_process_pcm16(&env, frame, FRAME_SAMPLES, &level), ZV_OK);
        ZV_CHECK(level <= previous);
        previous = level;
        frames_to_close++;
    }
    ZV_CHECK_EQ(level, 0);
    ZV_CHECK(frames_to_close > 5); /* release is slower than attack */

    zv_envelope_config_t bad = cfg;
    bad.full_scale_rms = bad.noise_floor_rms;
    ZV_CHECK_EQ(zv_envelope_init(&env, &bad), ZV_ERR_INVALID_ARG);
    ZV_CHECK_EQ(zv_envelope_process_pcm16(&env, frame, 0, &level), ZV_ERR_INVALID_ARG);
}

static void test_viseme_names(void)
{
    for (int v = 0; v < (int)ZV_VISEME_COUNT; v++) {
        zv_viseme_t parsed = ZV_VISEME_COUNT;
        ZV_CHECK_EQ(zv_viseme_from_name(zv_viseme_name((zv_viseme_t)v), &parsed), ZV_OK);
        ZV_CHECK_EQ(parsed, v);
    }
    zv_viseme_t parsed = ZV_VISEME_SIL;
    ZV_CHECK_EQ(zv_viseme_from_name("xx", &parsed), ZV_ERR_UNSUPPORTED);
    ZV_CHECK_STR(zv_viseme_name(ZV_VISEME_CH_SH), "ch_sh");
}

static void test_track_and_mouth(void)
{
    zv_viseme_track_t track;
    zv_viseme_track_reset(&track);
    ZV_CHECK_EQ(zv_viseme_track_push(&track, 100, ZV_VISEME_AA), ZV_OK);
    ZV_CHECK_EQ(zv_viseme_track_push(&track, 300, ZV_VISEME_MBP), ZV_OK);
    ZV_CHECK_EQ(zv_viseme_track_push(&track, 200, ZV_VISEME_EE), ZV_ERR_INVALID_ARG);

    zv_mouth_shape_t m;
    ZV_CHECK_EQ(zv_lipsync_mouth(&track, 150, 1000, true, &m), ZV_OK);
    ZV_CHECK_EQ(m.open, 1000);
    ZV_CHECK_EQ(zv_lipsync_mouth(&track, 150, 0, true, &m), ZV_OK);
    ZV_CHECK_EQ(m.open, 400);
    ZV_CHECK_EQ(zv_lipsync_mouth(&track, 310, 1000, true, &m), ZV_OK);
    ZV_CHECK_EQ(m.open, 0);

    /* Stale cue falls back to amplitude-only. */
    ZV_CHECK_EQ(zv_lipsync_mouth(&track, 300 + ZV_VISEME_STALE_MS + 1u, 700, true, &m), ZV_OK);
    ZV_CHECK_EQ(m.open, 700);
    /* Visemes disabled: amplitude-only even with fresh cues. */
    ZV_CHECK_EQ(zv_lipsync_mouth(&track, 150, 250, false, &m), ZV_OK);
    ZV_CHECK_EQ(m.open, 250);
    /* Before the first cue: amplitude-only. */
    ZV_CHECK_EQ(zv_lipsync_mouth(&track, 50, 333, true, &m), ZV_OK);
    ZV_CHECK_EQ(m.open, 333);

    zv_viseme_track_reset(&track);
    for (uint32_t i = 0; i < ZV_VISEME_TRACK_CAPACITY + 5u; i++) {
        ZV_CHECK_EQ(zv_viseme_track_push(&track, i * 10u, ZV_VISEME_OH), ZV_OK);
    }
    ZV_CHECK_EQ(track.count, ZV_VISEME_TRACK_CAPACITY);
    ZV_CHECK_EQ(track.cues[track.head].start_ms, 50);
    ZV_CHECK_EQ(zv_lipsync_mouth(NULL, 0, 0, true, &m), ZV_ERR_INVALID_ARG);
}

void test_suite_lipsync(void)
{
    test_envelope();
    test_viseme_names();
    test_track_and_mouth();
}
