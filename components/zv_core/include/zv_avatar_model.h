/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "zv_app_state.h"
#include "zv_lipsync.h"
#include "zv_status.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Brand colors (Zentari visual identity) used by the avatar ring. */
#define ZV_COLOR_ZENTARI_BLUE 0x1F75FEu
#define ZV_COLOR_ACCENT_BLUE 0x4F81BDu
#define ZV_COLOR_NAVY 0x0B1F33u
#define ZV_COLOR_SUCCESS 0x178A5Bu
#define ZV_COLOR_ERROR 0xC83E4Du
#define ZV_COLOR_APPROVAL 0xF4B400u
#define ZV_COLOR_BACKGROUND 0x04080Du

/** Static per-state expression. Geometry in permille; curves are signed permille. */
typedef struct {
    uint16_t eye_open;
    int16_t eye_curve; /**< -1000 worried lid .. +1000 happy arc */
    int16_t gaze_x;
    int16_t gaze_y;
    uint16_t mouth_open;
    uint16_t mouth_width;
    int16_t mouth_curve; /**< -1000 frown .. +1000 smile */
    uint32_t ring_rgb;
    uint16_t ring_intensity;
    uint16_t ring_period_ms; /**< 0 = steady */
    bool ring_sweep;         /**< Rotating arc segment instead of a full pulse */
    bool blink_enabled;
    bool lipsync_enabled;
} zv_expression_t;

/** Renderer-agnostic frame consumed by the LVGL face and mirrored by the browser prototype. */
typedef struct {
    uint16_t eye_open;
    int16_t eye_curve;
    int16_t gaze_x;
    int16_t gaze_y;
    uint16_t mouth_open;
    uint16_t mouth_width;
    uint16_t mouth_round;
    int16_t mouth_curve;
    uint32_t ring_rgb;
    uint16_t ring_intensity;
    uint16_t ring_sweep_deg;
    bool ring_sweep;
} zv_face_frame_t;

typedef struct {
    zv_avatar_state_t state;
    uint32_t state_entered_ms;
    uint32_t blink_wait_start_ms;
    uint32_t blink_wait_ms;
    uint32_t blink_started_ms;
    bool blinking;
    uint32_t rng;
} zv_avatar_model_t;

#define ZV_AVATAR_BLINK_DURATION_MS 160u
#define ZV_AVATAR_BLINK_MIN_INTERVAL_MS 2500u
#define ZV_AVATAR_BLINK_JITTER_MS 3500u

const zv_expression_t *zv_avatar_expression(zv_avatar_state_t state);
zv_status_t zv_avatar_init(zv_avatar_model_t *model, uint32_t now_ms, uint32_t seed);
zv_status_t zv_avatar_set_state(zv_avatar_model_t *model, zv_avatar_state_t state, uint32_t now_ms);

/**
 * Computes the face for `now_ms`. `mouth` may be NULL when no lip-sync data
 * is available; `level_permille` drives ring intensity while speaking.
 */
zv_status_t zv_avatar_frame(zv_avatar_model_t *model, uint32_t now_ms, const zv_mouth_shape_t *mouth,
                            uint16_t level_permille, zv_face_frame_t *out);

#ifdef __cplusplus
}
#endif
