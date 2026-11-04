/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 *
 * Portable, allocation-free audio acceptance helpers for the M1 gate.
 *
 * Two deliberate safety properties live here so they can be proven by host
 * tests without hardware:
 *
 *  1. The microphone probe is a streaming accumulator. It holds counters only,
 *     never a sample buffer, so an activity/RMS measurement structurally cannot
 *     record, store, transmit, or retain captured audio.
 *  2. Diagnostic tone generation clamps amplitude to
 *     ZV_AUDIO_TONE_MAX_AMPLITUDE_PERMILLE and applies a fade envelope, so a
 *     bounded diagnostic can never emit a full-scale or hard-edged burst.
 *
 * Integer-only, matching the rest of zv_core. No ESP-IDF or FreeRTOS headers.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "zv_status.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Levels are permille (0..1000) of PCM16 full scale. */
#define ZV_AUDIO_PERMILLE_FULL_SCALE 1000u

/**
 * Hard ceiling for diagnostic tone amplitude. 200 permille is -14 dBFS, loud
 * enough to be plainly audible on the NS4150B and far below full scale. The
 * clamp is enforced in code, not by the caller.
 */
#define ZV_AUDIO_TONE_MAX_AMPLITUDE_PERMILLE 200u

/** Longest diagnostic tone the generator will produce, in milliseconds. */
#define ZV_AUDIO_TONE_MAX_DURATION_MS 2000u

/**
 * Streaming microphone activity accumulator.
 *
 * Contains counters only. There is no sample storage field, and none may be
 * added: retaining captured audio is outside the M1 gate.
 */
typedef struct {
    uint64_t sum_squares;  /**< Sum of squared PCM16-scaled magnitudes */
    uint64_t sample_count; /**< Samples folded in so far */
    uint16_t peak_permille;/**< Largest single-sample magnitude seen */
    uint16_t rms_permille; /**< Valid only after zv_audio_probe_finish() */
    bool finished;
} zv_audio_probe_t;

/** Clears the accumulator. Safe to call repeatedly. */
void zv_audio_probe_reset(zv_audio_probe_t *probe);

/**
 * Folds one block of left-justified 32-bit I2S samples into the accumulator.
 *
 * The block is read once and never copied. The caller owns the buffer and is
 * expected to zero and release it immediately afterwards.
 */
zv_status_t zv_audio_probe_feed_i32(zv_audio_probe_t *probe, const int32_t *samples, size_t count);

/** Computes rms_permille/peak_permille and marks the probe finished. */
zv_status_t zv_audio_probe_finish(zv_audio_probe_t *probe);

/**
 * True when the finished probe observed activity at or above floor_permille.
 * An unfinished or empty probe is never active: the gate fails closed.
 */
bool zv_audio_probe_is_active(const zv_audio_probe_t *probe, uint16_t floor_permille);

/**
 * Renders a finite diagnostic sine into a left-justified 32-bit interleaved
 * buffer.
 *
 * amplitude_permille is clamped to ZV_AUDIO_TONE_MAX_AMPLITUDE_PERMILLE. A
 * linear fade is applied across fade_frames at each end so the amplifier
 * is never handed a step discontinuity. `phase` carries sample position across
 * calls and must be non-NULL.
 */
zv_status_t zv_audio_tone_fill_i32(int32_t *dst, size_t frames, uint8_t channels, uint32_t sample_rate_hz,
                                   uint32_t tone_hz, uint16_t amplitude_permille, size_t total_frames,
                                   size_t fade_frames, uint32_t *phase);

/** Clamp applied by zv_audio_tone_fill_i32(), exposed for callers and tests. */
uint16_t zv_audio_tone_clamp_permille(uint16_t amplitude_permille);

/** Frame count for a bounded tone, clamped to ZV_AUDIO_TONE_MAX_DURATION_MS. */
size_t zv_audio_tone_frame_count(uint32_t sample_rate_hz, uint32_t duration_ms);

#ifdef __cplusplus
}
#endif
