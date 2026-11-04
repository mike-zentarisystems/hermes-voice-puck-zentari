/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 */
#include "zv_avatar_model.h"

#include <stddef.h>

/*
 * Expression table. The browser prototype (prototype/avatar.js) mirrors the
 * ring colors; scripts/check_state_parity.py fails CI if they diverge.
 * Fields: eye_open, eye_curve, gaze_x, gaze_y, mouth_open, mouth_width,
 *         mouth_curve, ring_rgb, ring_intensity, ring_period_ms, ring_sweep,
 *         blink_enabled, lipsync_enabled
 */
static const zv_expression_t k_expressions[ZV_AVATAR_STATE_COUNT] = {
    [ZV_AVATAR_IDLE] = {850u, 0, 0, 0, 40u, 420u, 250, 0x1F75FEu, 450u, 4000u, false, true, false},
    [ZV_AVATAR_LISTENING] = {1000u, 0, 0, -80, 60u, 380u, 150, 0x1F75FEu, 1000u, 1200u, false, true, false},
    [ZV_AVATAR_THINKING] = {700u, -150, 350, -350, 30u, 300u, 0, 0x4F81BDu, 700u, 1800u, true, true, false},
    [ZV_AVATAR_SPEAKING] = {900u, 150, 0, 0, 0u, 520u, 200, 0x1F75FEu, 800u, 0u, false, true, true},
    [ZV_AVATAR_EXECUTING] = {800u, -100, 0, 0, 20u, 360u, 0, 0x1F75FEu, 900u, 900u, true, false, false},
    [ZV_AVATAR_SUCCESS] = {650u, 700, 0, 0, 120u, 560u, 850, 0x178A5Bu, 1000u, 0u, false, true, false},
    [ZV_AVATAR_ERROR] = {750u, -450, 0, 150, 60u, 420u, -600, 0xC83E4Du, 900u, 2000u, false, true, false},
    [ZV_AVATAR_APPROVAL] = {1000u, 0, 0, 0, 30u, 360u, -100, 0xF4B400u, 1000u, 1000u, false, false, false},
};

const zv_expression_t *zv_avatar_expression(zv_avatar_state_t state)
{
    if ((unsigned)state >= (unsigned)ZV_AVATAR_STATE_COUNT) {
        return NULL;
    }
    return &k_expressions[state];
}

static uint32_t xorshift32(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static void schedule_blink(zv_avatar_model_t *model, uint32_t now_ms)
{
    model->blink_wait_start_ms = now_ms;
    model->blink_wait_ms = ZV_AVATAR_BLINK_MIN_INTERVAL_MS + (xorshift32(&model->rng) % ZV_AVATAR_BLINK_JITTER_MS);
}

/* Triangle wave 0..1000..0 over `period_ms`. */
static uint32_t triangle(uint32_t elapsed_ms, uint32_t period_ms)
{
    if (period_ms < 2u) {
        return 0u;
    }
    uint32_t phase = elapsed_ms % period_ms;
    uint32_t half = period_ms / 2u;
    if (phase < half) {
        return (phase * 1000u) / half;
    }
    return ((period_ms - phase) * 1000u) / (period_ms - half);
}

zv_status_t zv_avatar_init(zv_avatar_model_t *model, uint32_t now_ms, uint32_t seed)
{
    if (model == NULL) {
        return ZV_ERR_INVALID_ARG;
    }
    model->state = ZV_AVATAR_IDLE;
    model->state_entered_ms = now_ms;
    model->blinking = false;
    model->blink_started_ms = 0u;
    model->rng = (seed == 0u) ? 0x9E3779B9u : seed;
    schedule_blink(model, now_ms);
    return ZV_OK;
}

zv_status_t zv_avatar_set_state(zv_avatar_model_t *model, zv_avatar_state_t state, uint32_t now_ms)
{
    if (model == NULL || (unsigned)state >= (unsigned)ZV_AVATAR_STATE_COUNT) {
        return ZV_ERR_INVALID_ARG;
    }
    if (model->state != state) {
        model->state = state;
        model->state_entered_ms = now_ms;
    }
    return ZV_OK;
}

static uint16_t clamp_permille(uint32_t value)
{
    return (uint16_t)(value > 1000u ? 1000u : value);
}

zv_status_t zv_avatar_frame(zv_avatar_model_t *model, uint32_t now_ms, const zv_mouth_shape_t *mouth,
                            uint16_t level_permille, zv_face_frame_t *out)
{
    if (model == NULL || out == NULL || (unsigned)model->state >= (unsigned)ZV_AVATAR_STATE_COUNT) {
        return ZV_ERR_INVALID_ARG;
    }
    const zv_expression_t *e = &k_expressions[model->state];
    uint32_t in_state = (uint32_t)(now_ms - model->state_entered_ms);

    out->eye_open = e->eye_open;
    out->eye_curve = e->eye_curve;
    out->gaze_x = e->gaze_x;
    out->gaze_y = e->gaze_y;
    out->mouth_open = e->mouth_open;
    out->mouth_width = e->mouth_width;
    out->mouth_round = 0u;
    out->mouth_curve = e->mouth_curve;
    out->ring_rgb = e->ring_rgb;
    out->ring_intensity = e->ring_intensity;
    out->ring_sweep = e->ring_sweep;
    out->ring_sweep_deg = 0u;

    if (e->lipsync_enabled && mouth != NULL) {
        out->mouth_open = clamp_permille(mouth->open);
        out->mouth_width = clamp_permille(mouth->width);
        out->mouth_round = clamp_permille(mouth->round);
    }

    if (e->ring_period_ms > 0u) {
        if (e->ring_sweep) {
            out->ring_sweep_deg = (uint16_t)(((in_state % e->ring_period_ms) * 360u) / e->ring_period_ms);
        } else {
            uint32_t wave = triangle(in_state, e->ring_period_ms);
            out->ring_intensity = clamp_permille(((uint32_t)e->ring_intensity * (600u + (wave * 4u) / 10u)) / 1000u);
        }
    }
    if (model->state == ZV_AVATAR_SPEAKING) {
        uint32_t floor_level = e->ring_intensity / 2u;
        out->ring_intensity = clamp_permille(level_permille > floor_level ? level_permille : floor_level);
    }
    if (model->state == ZV_AVATAR_THINKING) {
        out->gaze_x = (int16_t)(out->gaze_x + ((int32_t)triangle(in_state, 3200u) - 500) / 4);
    }

    if (e->blink_enabled) {
        if (!model->blinking && (uint32_t)(now_ms - model->blink_wait_start_ms) >= model->blink_wait_ms) {
            model->blinking = true;
            model->blink_started_ms = now_ms;
        }
        if (model->blinking) {
            uint32_t t = (uint32_t)(now_ms - model->blink_started_ms);
            if (t >= ZV_AVATAR_BLINK_DURATION_MS) {
                model->blinking = false;
                schedule_blink(model, now_ms);
            } else {
                uint32_t closure = triangle(t, ZV_AVATAR_BLINK_DURATION_MS);
                out->eye_open = (uint16_t)(((uint32_t)out->eye_open * (1000u - closure)) / 1000u);
            }
        }
    }
    return ZV_OK;
}
