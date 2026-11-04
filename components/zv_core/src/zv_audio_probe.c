/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 */
#include "zv_audio_probe.h"

#define SINE_TABLE_LEN 256u
#define PCM16_FULL_SCALE 32768

/* Q15 sine, one full period. Generated, not copied from upstream. */
static const int16_t k_sine_q15[SINE_TABLE_LEN] = {
         0,    804,   1608,   2410,   3212,   4011,   4808,   5602,
      6393,   7179,   7962,   8739,   9512,  10278,  11039,  11793,
     12539,  13279,  14010,  14732,  15446,  16151,  16846,  17530,
     18204,  18868,  19519,  20159,  20787,  21403,  22005,  22594,
     23170,  23731,  24279,  24811,  25329,  25832,  26319,  26790,
     27245,  27683,  28105,  28510,  28898,  29268,  29621,  29956,
     30273,  30571,  30852,  31113,  31356,  31580,  31785,  31971,
     32137,  32285,  32412,  32521,  32609,  32678,  32728,  32757,
     32767,  32757,  32728,  32678,  32609,  32521,  32412,  32285,
     32137,  31971,  31785,  31580,  31356,  31113,  30852,  30571,
     30273,  29956,  29621,  29268,  28898,  28510,  28105,  27683,
     27245,  26790,  26319,  25832,  25329,  24811,  24279,  23731,
     23170,  22594,  22005,  21403,  20787,  20159,  19519,  18868,
     18204,  17530,  16846,  16151,  15446,  14732,  14010,  13279,
     12539,  11793,  11039,  10278,   9512,   8739,   7962,   7179,
      6393,   5602,   4808,   4011,   3212,   2410,   1608,    804,
         0,   -804,  -1608,  -2410,  -3212,  -4011,  -4808,  -5602,
     -6393,  -7179,  -7962,  -8739,  -9512, -10278, -11039, -11793,
    -12539, -13279, -14010, -14732, -15446, -16151, -16846, -17530,
    -18204, -18868, -19519, -20159, -20787, -21403, -22005, -22594,
    -23170, -23731, -24279, -24811, -25329, -25832, -26319, -26790,
    -27245, -27683, -28105, -28510, -28898, -29268, -29621, -29956,
    -30273, -30571, -30852, -31113, -31356, -31580, -31785, -31971,
    -32137, -32285, -32412, -32521, -32609, -32678, -32728, -32757,
    -32767, -32757, -32728, -32678, -32609, -32521, -32412, -32285,
    -32137, -31971, -31785, -31580, -31356, -31113, -30852, -30571,
    -30273, -29956, -29621, -29268, -28898, -28510, -28105, -27683,
    -27245, -26790, -26319, -25832, -25329, -24811, -24279, -23731,
    -23170, -22594, -22005, -21403, -20787, -20159, -19519, -18868,
    -18204, -17530, -16846, -16151, -15446, -14732, -14010, -13279,
    -12539, -11793, -11039, -10278,  -9512,  -8739,  -7962,  -7179,
     -6393,  -5602,  -4808,  -4011,  -3212,  -2410,  -1608,   -804,
};

static uint64_t isqrt64(uint64_t value)
{
    uint64_t result = 0;
    uint64_t bit = (uint64_t)1 << 62;

    while (bit > value) {
        bit >>= 2;
    }
    while (bit != 0) {
        if (value >= result + bit) {
            value -= result + bit;
            result = (result >> 1) + bit;
        } else {
            result >>= 1;
        }
        bit >>= 2;
    }
    return result;
}

static uint16_t magnitude_to_permille(uint64_t magnitude)
{
    uint64_t permille = (magnitude * ZV_AUDIO_PERMILLE_FULL_SCALE) / (uint64_t)PCM16_FULL_SCALE;

    if (permille > ZV_AUDIO_PERMILLE_FULL_SCALE) {
        permille = ZV_AUDIO_PERMILLE_FULL_SCALE;
    }
    return (uint16_t)permille;
}

void zv_audio_probe_reset(zv_audio_probe_t *probe)
{
    if (probe == NULL) {
        return;
    }
    probe->sum_squares = 0;
    probe->sample_count = 0;
    probe->peak_permille = 0;
    probe->rms_permille = 0;
    probe->finished = false;
}

zv_status_t zv_audio_probe_feed_i32(zv_audio_probe_t *probe, const int32_t *samples, size_t count)
{
    if (probe == NULL || (samples == NULL && count > 0)) {
        return ZV_ERR_INVALID_ARG;
    }
    if (probe->finished) {
        return ZV_ERR_INVALID_STATE;
    }

    for (size_t i = 0; i < count; i++) {
        /*
         * ES7210 delivers left-justified 32-bit slots; the upper 16 bits carry
         * the sample. Folding to PCM16 keeps sum_squares inside uint64 for any
         * bounded acceptance run.
         */
        int32_t pcm16 = samples[i] >> 16;
        int64_t magnitude = (pcm16 < 0) ? -(int64_t)pcm16 : (int64_t)pcm16;

        probe->sum_squares += (uint64_t)(magnitude * magnitude);
        probe->sample_count++;

        uint16_t level = magnitude_to_permille((uint64_t)magnitude);
        if (level > probe->peak_permille) {
            probe->peak_permille = level;
        }
    }
    return ZV_OK;
}

zv_status_t zv_audio_probe_finish(zv_audio_probe_t *probe)
{
    if (probe == NULL) {
        return ZV_ERR_INVALID_ARG;
    }
    if (probe->finished) {
        return ZV_ERR_INVALID_STATE;
    }

    if (probe->sample_count == 0) {
        probe->rms_permille = 0;
    } else {
        probe->rms_permille = magnitude_to_permille(isqrt64(probe->sum_squares / probe->sample_count));
    }
    probe->finished = true;
    return ZV_OK;
}

bool zv_audio_probe_is_active(const zv_audio_probe_t *probe, uint16_t floor_permille)
{
    /* Fail closed: an unfinished or empty probe never reports activity. */
    if (probe == NULL || !probe->finished || probe->sample_count == 0) {
        return false;
    }
    return probe->rms_permille >= floor_permille;
}

uint16_t zv_audio_tone_clamp_permille(uint16_t amplitude_permille)
{
    return (amplitude_permille > ZV_AUDIO_TONE_MAX_AMPLITUDE_PERMILLE)
               ? (uint16_t)ZV_AUDIO_TONE_MAX_AMPLITUDE_PERMILLE
               : amplitude_permille;
}

size_t zv_audio_tone_frame_count(uint32_t sample_rate_hz, uint32_t duration_ms)
{
    if (sample_rate_hz == 0) {
        return 0;
    }
    uint32_t bounded_ms = (duration_ms > ZV_AUDIO_TONE_MAX_DURATION_MS) ? ZV_AUDIO_TONE_MAX_DURATION_MS : duration_ms;
    return (size_t)(((uint64_t)sample_rate_hz * bounded_ms) / 1000u);
}

static uint32_t fade_gain_permille(size_t frame_index, size_t total_frames, size_t fade_frames)
{
    if (frame_index >= total_frames) {
        return 0;
    }
    if (fade_frames == 0) {
        return ZV_AUDIO_PERMILLE_FULL_SCALE;
    }

    size_t remaining = total_frames - frame_index - 1u;
    size_t edge = (frame_index < remaining) ? frame_index : remaining;

    if (edge >= fade_frames) {
        return ZV_AUDIO_PERMILLE_FULL_SCALE;
    }
    return (uint32_t)(((uint64_t)(edge + 1u) * ZV_AUDIO_PERMILLE_FULL_SCALE) / (uint64_t)fade_frames);
}

zv_status_t zv_audio_tone_fill_i32(int32_t *dst, size_t frames, uint8_t channels, uint32_t sample_rate_hz,
                                   uint32_t tone_hz, uint16_t amplitude_permille, size_t total_frames,
                                   size_t fade_frames, uint32_t *phase)
{
    if (dst == NULL || phase == NULL || channels == 0 || sample_rate_hz == 0 || tone_hz == 0) {
        return ZV_ERR_INVALID_ARG;
    }
    if (tone_hz * 2u > sample_rate_hz) {
        return ZV_ERR_INVALID_ARG; /* above Nyquist */
    }

    uint16_t amplitude = zv_audio_tone_clamp_permille(amplitude_permille);
    size_t base = (size_t)*phase;

    for (size_t i = 0; i < frames; i++) {
        size_t frame_index = base + i;
        uint64_t step = ((uint64_t)frame_index * (uint64_t)tone_hz * (uint64_t)SINE_TABLE_LEN) / (uint64_t)sample_rate_hz;
        int32_t sine = k_sine_q15[(size_t)(step % (uint64_t)SINE_TABLE_LEN)];

        int64_t scaled = (int64_t)sine * (int64_t)amplitude / (int64_t)ZV_AUDIO_PERMILLE_FULL_SCALE;
        scaled = scaled * (int64_t)fade_gain_permille(frame_index, total_frames, fade_frames) /
                 (int64_t)ZV_AUDIO_PERMILLE_FULL_SCALE;

        for (uint8_t ch = 0; ch < channels; ch++) {
            dst[i * channels + ch] = (int32_t)((int32_t)scaled << 16);
        }
    }

    *phase = (uint32_t)(base + frames);
    return ZV_OK;
}
