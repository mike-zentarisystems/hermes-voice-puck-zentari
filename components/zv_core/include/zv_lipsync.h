/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "zv_status.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Reduced viseme set. Names (zv_viseme_name) are wire values in the
 * avatar.cue message and must match protocol/v1 schemas.
 */
typedef enum {
    ZV_VISEME_SIL = 0,
    ZV_VISEME_AA,
    ZV_VISEME_EE,
    ZV_VISEME_OH,
    ZV_VISEME_OO,
    ZV_VISEME_MBP,
    ZV_VISEME_FV,
    ZV_VISEME_L_TH,
    ZV_VISEME_CH_SH,
    ZV_VISEME_COUNT
} zv_viseme_t;

/** Mouth geometry in permille (0..1000). */
typedef struct {
    uint16_t open;
    uint16_t width;
    uint16_t round;
} zv_mouth_shape_t;

/** Integer-only amplitude envelope follower for lip-sync. */
typedef struct {
    uint16_t attack_permille;  /**< Fraction of the gap closed per frame when rising */
    uint16_t release_permille; /**< Fraction of the gap closed per frame when falling */
    uint16_t noise_floor_rms;  /**< PCM16 RMS at or below which the mouth is closed */
    uint16_t full_scale_rms;   /**< PCM16 RMS mapped to a fully open mouth */
} zv_envelope_config_t;

typedef struct {
    zv_envelope_config_t cfg;
    uint32_t level_permille;
} zv_envelope_t;

/** Staleness window after which a viseme cue no longer drives the mouth. */
#define ZV_VISEME_STALE_MS 350u
#define ZV_VISEME_TRACK_CAPACITY 32u

typedef struct {
    uint32_t start_ms; /**< Playback clock position of the cue */
    zv_viseme_t viseme;
} zv_viseme_cue_t;

typedef struct {
    zv_viseme_cue_t cues[ZV_VISEME_TRACK_CAPACITY];
    size_t head;
    size_t count;
} zv_viseme_track_t;

zv_envelope_config_t zv_envelope_default_config(void);
zv_status_t zv_envelope_init(zv_envelope_t *env, const zv_envelope_config_t *cfg);

/** Processes one PCM16 frame and writes the smoothed level (0..1000). */
zv_status_t zv_envelope_process_pcm16(zv_envelope_t *env, const int16_t *samples, size_t count,
                                      uint16_t *level_permille);

const char *zv_viseme_name(zv_viseme_t viseme);
zv_status_t zv_viseme_from_name(const char *name, zv_viseme_t *out);

void zv_viseme_track_reset(zv_viseme_track_t *track);

/** Cues must be pushed in non-decreasing start order; the oldest is dropped when full. */
zv_status_t zv_viseme_track_push(zv_viseme_track_t *track, uint32_t start_ms, zv_viseme_t viseme);

/**
 * Mouth shape for a playback position. Uses the latest non-stale viseme cue
 * (scaled by amplitude) when visemes are enabled; otherwise falls back to
 * amplitude-only lip-sync.
 */
zv_status_t zv_lipsync_mouth(const zv_viseme_track_t *track, uint32_t playback_ms, uint16_t amplitude_permille,
                             bool visemes_enabled, zv_mouth_shape_t *out);

#ifdef __cplusplus
}
#endif
