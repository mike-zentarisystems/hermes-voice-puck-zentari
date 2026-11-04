/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "zv_event.h"
#include "zv_status.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Application states (superset of the eight user-facing avatar states). */
typedef enum {
    ZV_STATE_BOOT = 0,
    ZV_STATE_PROVISIONING,
    ZV_STATE_CONNECTING,
    ZV_STATE_IDLE,
    ZV_STATE_LISTENING,
    ZV_STATE_THINKING,
    ZV_STATE_SPEAKING,
    ZV_STATE_EXECUTING,
    ZV_STATE_APPROVAL,
    ZV_STATE_SUCCESS,
    ZV_STATE_ERROR,
    ZV_STATE_DEGRADED,
    ZV_STATE_UPDATING,
    ZV_STATE_COUNT
} zv_app_state_t;

/**
 * User-facing avatar states. The names returned by zv_avatar_state_name()
 * are part of the wire protocol (device.state.avatar_state) and the browser
 * prototype; scripts/check_state_parity.py enforces that all three agree.
 */
typedef enum {
    ZV_AVATAR_IDLE = 0,
    ZV_AVATAR_LISTENING,
    ZV_AVATAR_THINKING,
    ZV_AVATAR_SPEAKING,
    ZV_AVATAR_EXECUTING,
    ZV_AVATAR_SUCCESS,
    ZV_AVATAR_ERROR,
    ZV_AVATAR_APPROVAL,
    ZV_AVATAR_STATE_COUNT
} zv_avatar_state_t;

/**
 * Pure transition function.
 * - Telemetry events (audio level, viseme, notification) return ZV_OK and
 *   leave the state unchanged.
 * - Events with no rule for the current state return
 *   ZV_ERR_INVALID_TRANSITION and leave *next equal to current.
 */
zv_status_t zv_app_state_next(zv_app_state_t current, const zv_event_t *event, zv_app_state_t *next);

/** Avatar state presented for an application state. */
zv_avatar_state_t zv_app_state_to_avatar(zv_app_state_t state);

const char *zv_app_state_name(zv_app_state_t state);
const char *zv_avatar_state_name(zv_avatar_state_t state);

#ifdef __cplusplus
}
#endif
