/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 */
#include "zv_cred_rotation.h"

#include <stddef.h>

zv_status_t zv_rotation_init(zv_rotation_t *rot, uint32_t active_generation)
{
    if (rot == NULL) {
        return ZV_ERR_INVALID_ARG;
    }
    rot->state = ZV_ROT_IDLE;
    rot->active_generation = active_generation;
    rot->staged_generation = 0u;
    rot->verify_started_ms = 0u;
    rot->verify_timeout_ms = 0u;
    return ZV_OK;
}

zv_status_t zv_rotation_stage(zv_rotation_t *rot, uint32_t new_generation, uint32_t verify_timeout_ms)
{
    if (rot == NULL || verify_timeout_ms == 0u) {
        return ZV_ERR_INVALID_ARG;
    }
    if (rot->state == ZV_ROT_STAGED || rot->state == ZV_ROT_VERIFYING) {
        return ZV_ERR_INVALID_STATE;
    }
    if (new_generation <= rot->active_generation) {
        return ZV_ERR_POLICY;
    }
    rot->state = ZV_ROT_STAGED;
    rot->staged_generation = new_generation;
    rot->verify_timeout_ms = verify_timeout_ms;
    return ZV_OK;
}

zv_status_t zv_rotation_begin_verify(zv_rotation_t *rot, uint32_t now_ms)
{
    if (rot == NULL) {
        return ZV_ERR_INVALID_ARG;
    }
    if (rot->state != ZV_ROT_STAGED) {
        return ZV_ERR_INVALID_STATE;
    }
    rot->state = ZV_ROT_VERIFYING;
    rot->verify_started_ms = now_ms;
    return ZV_OK;
}

zv_status_t zv_rotation_on_verified(zv_rotation_t *rot)
{
    if (rot == NULL) {
        return ZV_ERR_INVALID_ARG;
    }
    if (rot->state != ZV_ROT_VERIFYING) {
        return ZV_ERR_INVALID_STATE;
    }
    rot->active_generation = rot->staged_generation;
    rot->staged_generation = 0u;
    rot->state = ZV_ROT_COMMITTED;
    return ZV_OK;
}

zv_status_t zv_rotation_on_failed(zv_rotation_t *rot)
{
    if (rot == NULL) {
        return ZV_ERR_INVALID_ARG;
    }
    if (rot->state != ZV_ROT_VERIFYING && rot->state != ZV_ROT_STAGED) {
        return ZV_ERR_INVALID_STATE;
    }
    rot->staged_generation = 0u;
    rot->state = ZV_ROT_ROLLED_BACK;
    return ZV_OK;
}

zv_rotation_state_t zv_rotation_tick(zv_rotation_t *rot, uint32_t now_ms)
{
    if (rot == NULL) {
        return ZV_ROT_ROLLED_BACK;
    }
    if (rot->state == ZV_ROT_VERIFYING && (uint32_t)(now_ms - rot->verify_started_ms) >= rot->verify_timeout_ms) {
        rot->staged_generation = 0u;
        rot->state = ZV_ROT_ROLLED_BACK;
    }
    return rot->state;
}
