/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 */
#include "zv_lipsync.h"

#include <stddef.h>
#include <string.h>

static const char *const k_viseme_names[ZV_VISEME_COUNT] = {
    [ZV_VISEME_SIL] = "sil", [ZV_VISEME_AA] = "aa",     [ZV_VISEME_EE] = "ee",
    [ZV_VISEME_OH] = "oh",   [ZV_VISEME_OO] = "oo",     [ZV_VISEME_MBP] = "mbp",
    [ZV_VISEME_FV] = "fv",   [ZV_VISEME_L_TH] = "l_th", [ZV_VISEME_CH_SH] = "ch_sh",
};

/* open, width, round (permille) */
static const zv_mouth_shape_t k_viseme_shapes[ZV_VISEME_COUNT] = {
    [ZV_VISEME_SIL] = {0u, 450u, 200u},   [ZV_VISEME_AA] = {1000u, 600u, 150u},
    [ZV_VISEME_EE] = {450u, 900u, 0u},    [ZV_VISEME_OH] = {750u, 400u, 800u},
    [ZV_VISEME_OO] = {400u, 250u, 1000u}, [ZV_VISEME_MBP] = {0u, 500u, 100u},
    [ZV_VISEME_FV] = {200u, 650u, 0u},    [ZV_VISEME_L_TH] = {500u, 550u, 100u},
    [ZV_VISEME_CH_SH] = {350u, 500u, 600u},
};

zv_envelope_config_t zv_envelope_default_config(void)
{
    zv_envelope_config_t cfg = {
        .attack_permille = 600u,
        .release_permille = 150u,
        .noise_floor_rms = 300u,
        .full_scale_rms = 12000u,
    };
    return cfg;
}

zv_status_t zv_envelope_init(zv_envelope_t *env, const zv_envelope_config_t *cfg)
{
    if (env == NULL || cfg == NULL || cfg->attack_permille == 0u || cfg->attack_permille > 1000u ||
        cfg->release_permille == 0u || cfg->release_permille > 1000u || cfg->full_scale_rms <= cfg->noise_floor_rms) {
        return ZV_ERR_INVALID_ARG;
    }
    env->cfg = *cfg;
    env->level_permille = 0u;
    return ZV_OK;
}

static uint32_t isqrt64(uint64_t value)
{
    uint64_t result = 0u;
    uint64_t bit = (uint64_t)1 << 62;
    while (bit > value) {
        bit >>= 2;
    }
    while (bit != 0u) {
        if (value >= result + bit) {
            value -= result + bit;
            result = (result >> 1) + bit;
        } else {
            result >>= 1;
        }
        bit >>= 2;
    }
    return (uint32_t)result;
}

zv_status_t zv_envelope_process_pcm16(zv_envelope_t *env, const int16_t *samples, size_t count,
                                      uint16_t *level_permille)
{
    if (env == NULL || samples == NULL || count == 0u || level_permille == NULL) {
        return ZV_ERR_INVALID_ARG;
    }
    uint64_t sum_squares = 0u;
    for (size_t i = 0u; i < count; i++) {
        int32_t s = samples[i];
        sum_squares += (uint64_t)(s * s);
    }
    uint32_t rms = isqrt64(sum_squares / count);

    uint32_t target = 0u;
    if (rms > env->cfg.noise_floor_rms) {
        uint32_t span = (uint32_t)env->cfg.full_scale_rms - env->cfg.noise_floor_rms;
        uint32_t above = rms - env->cfg.noise_floor_rms;
        target = above >= span ? 1000u : (above * 1000u) / span;
    }

    uint32_t level = env->level_permille;
    if (target > level) {
        uint32_t step = ((target - level) * env->cfg.attack_permille) / 1000u;
        level += (step == 0u) ? 1u : step;
    } else if (target < level) {
        uint32_t step = ((level - target) * env->cfg.release_permille) / 1000u;
        level -= (step == 0u) ? 1u : step;
    }
    env->level_permille = level;
    *level_permille = (uint16_t)level;
    return ZV_OK;
}

const char *zv_viseme_name(zv_viseme_t viseme)
{
    if ((unsigned)viseme >= (unsigned)ZV_VISEME_COUNT) {
        return "unknown";
    }
    return k_viseme_names[viseme];
}

zv_status_t zv_viseme_from_name(const char *name, zv_viseme_t *out)
{
    if (name == NULL || out == NULL) {
        return ZV_ERR_INVALID_ARG;
    }
    for (unsigned i = 0u; i < (unsigned)ZV_VISEME_COUNT; i++) {
        if (strcmp(name, k_viseme_names[i]) == 0) {
            *out = (zv_viseme_t)i;
            return ZV_OK;
        }
    }
    return ZV_ERR_UNSUPPORTED;
}

void zv_viseme_track_reset(zv_viseme_track_t *track)
{
    if (track != NULL) {
        track->head = 0u;
        track->count = 0u;
    }
}

static const zv_viseme_cue_t *cue_at(const zv_viseme_track_t *track, size_t offset)
{
    return &track->cues[(track->head + offset) % ZV_VISEME_TRACK_CAPACITY];
}

zv_status_t zv_viseme_track_push(zv_viseme_track_t *track, uint32_t start_ms, zv_viseme_t viseme)
{
    if (track == NULL || (unsigned)viseme >= (unsigned)ZV_VISEME_COUNT) {
        return ZV_ERR_INVALID_ARG;
    }
    if (track->count > 0u && start_ms < cue_at(track, track->count - 1u)->start_ms) {
        return ZV_ERR_INVALID_ARG;
    }
    if (track->count == ZV_VISEME_TRACK_CAPACITY) {
        track->head = (track->head + 1u) % ZV_VISEME_TRACK_CAPACITY;
        track->count--;
    }
    zv_viseme_cue_t *slot = &track->cues[(track->head + track->count) % ZV_VISEME_TRACK_CAPACITY];
    slot->start_ms = start_ms;
    slot->viseme = viseme;
    track->count++;
    return ZV_OK;
}

zv_status_t zv_lipsync_mouth(const zv_viseme_track_t *track, uint32_t playback_ms, uint16_t amplitude_permille,
                             bool visemes_enabled, zv_mouth_shape_t *out)
{
    if (out == NULL || (visemes_enabled && track == NULL)) {
        return ZV_ERR_INVALID_ARG;
    }
    uint32_t amp = amplitude_permille > 1000u ? 1000u : amplitude_permille;

    if (visemes_enabled) {
        const zv_viseme_cue_t *active = NULL;
        for (size_t i = 0u; i < track->count; i++) {
            const zv_viseme_cue_t *cue = cue_at(track, i);
            if (cue->start_ms > playback_ms) {
                break;
            }
            active = cue;
        }
        if (active != NULL && (playback_ms - active->start_ms) <= ZV_VISEME_STALE_MS) {
            zv_mouth_shape_t shape = k_viseme_shapes[active->viseme];
            /* Amplitude modulates the viseme opening between 40% and 100%. */
            shape.open = (uint16_t)(((uint32_t)shape.open * (400u + (amp * 6u) / 10u)) / 1000u);
            *out = shape;
            return ZV_OK;
        }
    }

    out->open = (uint16_t)amp;
    out->width = (uint16_t)(450u + amp / 10u);
    out->round = 250u;
    return ZV_OK;
}
