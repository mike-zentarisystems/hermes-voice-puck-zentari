/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 *
 * Enforces the M1 audio safety boundaries that can be proven without hardware.
 */
#include "zv_audio_probe.h"
#include "zv_test.h"

#include <stdlib.h>

#define SAMPLE_RATE 16000u
#define CHANNELS 2u

static int32_t left_justify(int16_t pcm16)
{
    return (int32_t)pcm16 << 16;
}

static void test_probe_fails_closed(void)
{
    zv_audio_probe_t probe;

    zv_audio_probe_reset(&probe);

    /* Unfinished probe never reports activity. */
    ZV_CHECK(!zv_audio_probe_is_active(&probe, 0));

    /* Empty finished probe never reports activity, even at floor 0. */
    ZV_CHECK_EQ(zv_audio_probe_finish(&probe), ZV_OK);
    ZV_CHECK_EQ(probe.rms_permille, 0);
    ZV_CHECK(!zv_audio_probe_is_active(&probe, 0));

    /* NULL probe is never active. */
    ZV_CHECK(!zv_audio_probe_is_active(NULL, 0));

    /* Finishing twice is an explicit error, not a silent success. */
    ZV_CHECK_EQ(zv_audio_probe_finish(&probe), ZV_ERR_INVALID_STATE);

    /* Feeding after finish is rejected. */
    int32_t sample = left_justify(1000);
    ZV_CHECK_EQ(zv_audio_probe_feed_i32(&probe, &sample, 1), ZV_ERR_INVALID_STATE);
}

static void test_probe_rejects_bad_args(void)
{
    zv_audio_probe_t probe;
    int32_t sample = 0;

    zv_audio_probe_reset(&probe);
    ZV_CHECK_EQ(zv_audio_probe_feed_i32(NULL, &sample, 1), ZV_ERR_INVALID_ARG);
    ZV_CHECK_EQ(zv_audio_probe_feed_i32(&probe, NULL, 1), ZV_ERR_INVALID_ARG);
    ZV_CHECK_EQ(zv_audio_probe_finish(NULL), ZV_ERR_INVALID_ARG);

    /* A zero-length feed with a NULL buffer is a no-op, not an error. */
    ZV_CHECK_EQ(zv_audio_probe_feed_i32(&probe, NULL, 0), ZV_OK);
}

static void test_probe_measures_silence_and_tone(void)
{
    zv_audio_probe_t silent;
    zv_audio_probe_t loud;
    int32_t block[64];

    zv_audio_probe_reset(&silent);
    for (size_t i = 0; i < 64; i++) {
        block[i] = 0;
    }
    ZV_CHECK_EQ(zv_audio_probe_feed_i32(&silent, block, 64), ZV_OK);
    ZV_CHECK_EQ(zv_audio_probe_finish(&silent), ZV_OK);
    ZV_CHECK_EQ(silent.rms_permille, 0);
    ZV_CHECK_EQ(silent.peak_permille, 0);
    /* Silence must not pass a non-zero activity floor. */
    ZV_CHECK(!zv_audio_probe_is_active(&silent, 20));

    /* A square wave at half scale has RMS == peak == ~500 permille. */
    zv_audio_probe_reset(&loud);
    for (size_t i = 0; i < 64; i++) {
        block[i] = left_justify((int16_t)((i % 2 == 0) ? 16384 : -16384));
    }
    ZV_CHECK_EQ(zv_audio_probe_feed_i32(&loud, block, 64), ZV_OK);
    ZV_CHECK_EQ(zv_audio_probe_finish(&loud), ZV_OK);
    ZV_CHECK_EQ(loud.rms_permille, 500);
    ZV_CHECK_EQ(loud.peak_permille, 500);
    ZV_CHECK(zv_audio_probe_is_active(&loud, 20));
    ZV_CHECK(!zv_audio_probe_is_active(&loud, 900));
}

static void test_probe_accumulates_across_blocks(void)
{
    zv_audio_probe_t split;
    zv_audio_probe_t whole;
    int32_t block[32];

    for (size_t i = 0; i < 32; i++) {
        block[i] = left_justify((int16_t)((i % 2 == 0) ? 8192 : -8192));
    }

    zv_audio_probe_reset(&split);
    ZV_CHECK_EQ(zv_audio_probe_feed_i32(&split, block, 16), ZV_OK);
    ZV_CHECK_EQ(zv_audio_probe_feed_i32(&split, block + 16, 16), ZV_OK);
    ZV_CHECK_EQ(zv_audio_probe_finish(&split), ZV_OK);

    zv_audio_probe_reset(&whole);
    ZV_CHECK_EQ(zv_audio_probe_feed_i32(&whole, block, 32), ZV_OK);
    ZV_CHECK_EQ(zv_audio_probe_finish(&whole), ZV_OK);

    ZV_CHECK_EQ(split.rms_permille, whole.rms_permille);
    ZV_CHECK_EQ(split.sample_count, 32);
}

static void test_tone_amplitude_is_clamped(void)
{
    /* The clamp is enforced in code, not trusted to the caller. */
    ZV_CHECK_EQ(zv_audio_tone_clamp_permille(1000), ZV_AUDIO_TONE_MAX_AMPLITUDE_PERMILLE);
    ZV_CHECK_EQ(zv_audio_tone_clamp_permille(65535), ZV_AUDIO_TONE_MAX_AMPLITUDE_PERMILLE);
    ZV_CHECK_EQ(zv_audio_tone_clamp_permille(50), 50);
    uint32_t clamp = ZV_AUDIO_TONE_MAX_AMPLITUDE_PERMILLE;
    uint32_t full_scale = ZV_AUDIO_PERMILLE_FULL_SCALE;
    ZV_CHECK(clamp < full_scale);

    /* Even when asked for full scale, no rendered sample may exceed the clamp. */
    const size_t frames = 1600;
    int32_t *buffer = calloc(frames * CHANNELS, sizeof(int32_t));
    uint32_t phase = 0;
    int32_t ceiling = (int32_t)((32767 * (int32_t)ZV_AUDIO_TONE_MAX_AMPLITUDE_PERMILLE) / 1000) + 1;

    ZV_CHECK(buffer != NULL);
    if (buffer == NULL) {
        return;
    }
    ZV_CHECK_EQ(zv_audio_tone_fill_i32(buffer, frames, (uint8_t)CHANNELS, SAMPLE_RATE, 440, 1000, frames, 160, &phase),
                ZV_OK);

    int over = 0;
    for (size_t i = 0; i < frames * CHANNELS; i++) {
        int32_t pcm16 = buffer[i] >> 16;
        if (pcm16 > ceiling || pcm16 < -ceiling) {
            over++;
        }
    }
    ZV_CHECK_EQ(over, 0);
    free(buffer);
}

static void test_tone_fades_in_and_out(void)
{
    const size_t frames = 800;
    const size_t fade = 80;
    int32_t *buffer = calloc(frames * CHANNELS, sizeof(int32_t));
    uint32_t phase = 0;

    ZV_CHECK(buffer != NULL);
    if (buffer == NULL) {
        return;
    }
    ZV_CHECK_EQ(zv_audio_tone_fill_i32(buffer, frames, (uint8_t)CHANNELS, SAMPLE_RATE, 440, 200, frames, fade, &phase),
                ZV_OK);

    /* The amplifier is never handed a step: both edges start and end near zero. */
    int32_t first = buffer[0] >> 16;
    int32_t last = buffer[(frames - 1) * CHANNELS] >> 16;
    ZV_CHECK(first > -80 && first < 80);
    ZV_CHECK(last > -80 && last < 80);

    /* Phase advanced by exactly the frames written. */
    ZV_CHECK_EQ(phase, frames);

    /* Both channels carry the identical diagnostic signal. */
    int mismatched = 0;
    for (size_t i = 0; i < frames; i++) {
        if (buffer[i * CHANNELS] != buffer[i * CHANNELS + 1]) {
            mismatched++;
        }
    }
    ZV_CHECK_EQ(mismatched, 0);
    free(buffer);
}

static void test_tone_rejects_bad_args(void)
{
    int32_t buffer[8] = {0};
    uint32_t phase = 0;

    ZV_CHECK_EQ(zv_audio_tone_fill_i32(NULL, 4, 2, SAMPLE_RATE, 440, 100, 4, 1, &phase), ZV_ERR_INVALID_ARG);
    ZV_CHECK_EQ(zv_audio_tone_fill_i32(buffer, 4, 0, SAMPLE_RATE, 440, 100, 4, 1, &phase), ZV_ERR_INVALID_ARG);
    ZV_CHECK_EQ(zv_audio_tone_fill_i32(buffer, 4, 2, 0, 440, 100, 4, 1, &phase), ZV_ERR_INVALID_ARG);
    ZV_CHECK_EQ(zv_audio_tone_fill_i32(buffer, 4, 2, SAMPLE_RATE, 0, 100, 4, 1, &phase), ZV_ERR_INVALID_ARG);
    ZV_CHECK_EQ(zv_audio_tone_fill_i32(buffer, 4, 2, SAMPLE_RATE, 440, 100, 4, 1, NULL), ZV_ERR_INVALID_ARG);
    /* Above Nyquist is refused rather than aliased. */
    ZV_CHECK_EQ(zv_audio_tone_fill_i32(buffer, 4, 2, SAMPLE_RATE, SAMPLE_RATE, 100, 4, 1, &phase), ZV_ERR_INVALID_ARG);
}

static void test_tone_duration_is_bounded(void)
{
    ZV_CHECK_EQ(zv_audio_tone_frame_count(SAMPLE_RATE, 1000), 16000);
    ZV_CHECK_EQ(zv_audio_tone_frame_count(SAMPLE_RATE, 500), 8000);
    ZV_CHECK_EQ(zv_audio_tone_frame_count(0, 500), 0);

    /* A caller asking for a long tone gets the bounded maximum, not the ask. */
    size_t huge = zv_audio_tone_frame_count(SAMPLE_RATE, 600000);
    ZV_CHECK_EQ(huge, (size_t)((SAMPLE_RATE * ZV_AUDIO_TONE_MAX_DURATION_MS) / 1000u));
    uint32_t max_ms = ZV_AUDIO_TONE_MAX_DURATION_MS;
    ZV_CHECK(max_ms <= 2000u);
}

void test_suite_audio_probe(void)
{
    test_probe_fails_closed();
    test_probe_rejects_bad_args();
    test_probe_measures_silence_and_tone();
    test_probe_accumulates_across_blocks();
    test_tone_amplitude_is_clamped();
    test_tone_fades_in_and_out();
    test_tone_rejects_bad_args();
    test_tone_duration_is_bounded();
}
