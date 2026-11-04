/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 *
 * Output volume, mute state, and the repaint cadence the UI shell uses to keep
 * the clock honest. No ESP-IDF, no allocation, no I/O: the caller supplies the
 * time and applies zv_volume_effective() to the amplifier.
 *
 * Mute is stored as a flag beside the level, never as a level of zero. Writing
 * zero into the level on mute would destroy the operator's chosen volume, so
 * unmuting would return to silence instead of to the previous setting.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "zv_status.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZV_VOLUME_MAX 100u
/** One detent of the volume up/down action. */
#define ZV_VOLUME_STEP 10u
/** Indicator granularity, independent of the 0..100 level. */
#define ZV_VOLUME_BARS 10u

typedef struct {
    uint8_t level; /**< 0..100, remembered unchanged while muted */
    bool muted;
} zv_volume_t;

/**
 * Repaint cadence for time-dependent chrome. The clock is the only thing on the
 * panel that changes on its own, so repainting on a fixed period is sufficient
 * and keeps the redraw cost bounded.
 */
typedef struct {
    uint32_t period_ms;
    uint32_t last_ms;
    bool primed; /**< false until the first zv_repaint_mark() */
} zv_repaint_t;

/** Shortest accepted repaint period; anything lower is clamped to this. */
#define ZV_REPAINT_MIN_PERIOD_MS 1000u

void zv_volume_init(zv_volume_t *volume, uint8_t default_level);

uint8_t zv_volume_level(const zv_volume_t *volume);
bool zv_volume_is_muted(const zv_volume_t *volume);

/**
 * Level actually sent to the amplifier: 0 while muted, otherwise the remembered
 * level. This is the only value a caller should hand to the codec.
 */
uint8_t zv_volume_effective(const zv_volume_t *volume);

/** Sets the remembered level, clamped into 0..ZV_VOLUME_MAX. Mute is untouched. */
zv_status_t zv_volume_set_level(zv_volume_t *volume, uint8_t level);

/** Adds a signed delta, clamping at both ends. */
zv_status_t zv_volume_step(zv_volume_t *volume, int8_t delta);

zv_status_t zv_volume_set_muted(zv_volume_t *volume, bool muted);

/** Flips mute and returns the new state. The level is never modified. */
bool zv_volume_toggle_mute(zv_volume_t *volume);

/** Filled indicator segments, 0..ZV_VOLUME_BARS. Zero while muted. */
uint8_t zv_volume_bars(const zv_volume_t *volume);

const char *zv_mute_state_name(bool muted);

/** period_ms is clamped up to ZV_REPAINT_MIN_PERIOD_MS. */
void zv_repaint_init(zv_repaint_t *repaint, uint32_t period_ms);
uint32_t zv_repaint_period_ms(const zv_repaint_t *repaint);

/**
 * True once the period has elapsed since the last mark, and true immediately
 * before the first mark so the opening frame is always painted. The comparison
 * is done in uint32_t so a millisecond counter that wraps mid-interval is read
 * as elapsed time rather than as a huge negative delta.
 */
bool zv_repaint_due(const zv_repaint_t *repaint, uint32_t now_ms);

void zv_repaint_mark(zv_repaint_t *repaint, uint32_t now_ms);

#ifdef __cplusplus
}
#endif