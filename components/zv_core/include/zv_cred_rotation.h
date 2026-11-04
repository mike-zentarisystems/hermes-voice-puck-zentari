/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdint.h>

#include "zv_status.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Two-phase rotation of the scoped gateway credential:
 *   IDLE/COMMITTED/ROLLED_BACK --stage(gen > active)--> STAGED
 *   STAGED --begin_verify--> VERIFYING (reconnect using staged credential)
 *   VERIFYING --verified--> COMMITTED (old credential erased)
 *   VERIFYING --failed or timeout--> ROLLED_BACK (staged credential erased)
 * Generations are strictly monotonic so a replayed old credential is refused.
 */
typedef enum {
    ZV_ROT_IDLE = 0,
    ZV_ROT_STAGED,
    ZV_ROT_VERIFYING,
    ZV_ROT_COMMITTED,
    ZV_ROT_ROLLED_BACK,
} zv_rotation_state_t;

typedef struct {
    zv_rotation_state_t state;
    uint32_t active_generation;
    uint32_t staged_generation;
    uint32_t verify_started_ms;
    uint32_t verify_timeout_ms;
} zv_rotation_t;

zv_status_t zv_rotation_init(zv_rotation_t *rot, uint32_t active_generation);
zv_status_t zv_rotation_stage(zv_rotation_t *rot, uint32_t new_generation, uint32_t verify_timeout_ms);
zv_status_t zv_rotation_begin_verify(zv_rotation_t *rot, uint32_t now_ms);
zv_status_t zv_rotation_on_verified(zv_rotation_t *rot);
zv_status_t zv_rotation_on_failed(zv_rotation_t *rot);

/** Rolls back a verification that exceeded its timeout; returns the resulting state. */
zv_rotation_state_t zv_rotation_tick(zv_rotation_t *rot, uint32_t now_ms);

#ifdef __cplusplus
}
#endif
