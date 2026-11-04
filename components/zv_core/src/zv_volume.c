/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 */
#include "zv_volume.h"

#include <string.h>

void zv_volume_init(zv_volume_t *volume, uint8_t default_level)
{
    if (volume == NULL) {
        return;
    }
    memset(volume, 0, sizeof(*volume));
    volume->level = default_level > (uint8_t)ZV_VOLUME_MAX ? (uint8_t)ZV_VOLUME_MAX : default_level;
    volume->muted = false;
}

uint8_t zv_volume_level(const zv_volume_t *volume) { return volume == NULL ? 0u : volume->level; }

bool zv_volume_is_muted(const zv_volume_t *volume) { return volume != NULL && volume->muted; }

uint8_t zv_volume_effective(const zv_volume_t *volume)
{
    if (volume == NULL || volume->muted) {
        return 0u;
    }
    return volume->level;
}

zv_status_t zv_volume_set_level(zv_volume_t *volume, uint8_t level)
{
    if (volume == NULL) {
        return ZV_ERR_INVALID_ARG;
    }
    /* Clamp rather than reject: the slider drag computes a raw coordinate that
     * is legitimately out of range at the two extremes. */
    volume->level = level > (uint8_t)ZV_VOLUME_MAX ? (uint8_t)ZV_VOLUME_MAX : level;
    return ZV_OK;
}

zv_status_t zv_volume_step(zv_volume_t *volume, int8_t delta)
{
    if (volume == NULL) {
        return ZV_ERR_INVALID_ARG;
    }
    int32_t next = (int32_t)volume->level + (int32_t)delta;
    if (next < 0) {
        next = 0;
    }
    if (next > (int32_t)ZV_VOLUME_MAX) {
        next = (int32_t)ZV_VOLUME_MAX;
    }
    volume->level = (uint8_t)next;
    return ZV_OK;
}

zv_status_t zv_volume_set_muted(zv_volume_t *volume, bool muted)
{
    if (volume == NULL) {
        return ZV_ERR_INVALID_ARG;
    }
    /* Only the flag moves. The level stays exactly as the operator left it. */
    volume->muted = muted;
    return ZV_OK;
}

bool zv_volume_toggle_mute(zv_volume_t *volume)
{
    if (volume == NULL) {
        return false;
    }
    volume->muted = !volume->muted;
    return volume->muted;
}

uint8_t zv_volume_bars(const zv_volume_t *volume)
{
    if (volume == NULL || volume->muted) {
        return 0u;
    }
    /* Round to nearest so 55% shows 6 of 10 bars rather than truncating to 5. */
    uint32_t filled = ((uint32_t)volume->level * ZV_VOLUME_BARS + (ZV_VOLUME_MAX / 2u)) / ZV_VOLUME_MAX;
    if (filled > ZV_VOLUME_BARS) {
        filled = ZV_VOLUME_BARS;
    }
    return (uint8_t)filled;
}

const char *zv_mute_state_name(bool muted) { return muted ? "muted" : "unmuted"; }

void zv_repaint_init(zv_repaint_t *repaint, uint32_t period_ms)
{
    if (repaint == NULL) {
        return;
    }
    memset(repaint, 0, sizeof(*repaint));
    repaint->period_ms = period_ms < ZV_REPAINT_MIN_PERIOD_MS ? ZV_REPAINT_MIN_PERIOD_MS : period_ms;
    repaint->primed = false;
}

uint32_t zv_repaint_period_ms(const zv_repaint_t *repaint) { return repaint == NULL ? 0u : repaint->period_ms; }

bool zv_repaint_due(const zv_repaint_t *repaint, uint32_t now_ms)
{
    if (repaint == NULL) {
        return false;
    }
    if (!repaint->primed) {
        return true; /* the opening frame must paint regardless of the period */
    }
    return (uint32_t)(now_ms - repaint->last_ms) >= repaint->period_ms;
}

void zv_repaint_mark(zv_repaint_t *repaint, uint32_t now_ms)
{
    if (repaint == NULL) {
        return;
    }
    repaint->last_ms = now_ms;
    repaint->primed = true;
}