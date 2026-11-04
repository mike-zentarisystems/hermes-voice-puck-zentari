/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 */
#include "zv_circuit_breaker.h"

#include <stddef.h>

zv_status_t zv_cb_init(zv_circuit_breaker_t *breaker, const zv_cb_config_t *cfg)
{
    if (breaker == NULL || cfg == NULL || cfg->failure_threshold == 0u || cfg->open_duration_ms == 0u ||
        cfg->half_open_max_trials == 0u) {
        return ZV_ERR_INVALID_ARG;
    }
    breaker->cfg = *cfg;
    breaker->state = ZV_CB_CLOSED;
    breaker->consecutive_failures = 0u;
    breaker->opened_at_ms = 0u;
    breaker->half_open_trials = 0u;
    breaker->trip_count = 0u;
    return ZV_OK;
}

zv_status_t zv_cb_allow(zv_circuit_breaker_t *breaker, uint32_t now_ms)
{
    if (breaker == NULL) {
        return ZV_ERR_INVALID_ARG;
    }
    if (breaker->state == ZV_CB_CLOSED) {
        return ZV_OK;
    }
    if (breaker->state == ZV_CB_OPEN) {
        if ((uint32_t)(now_ms - breaker->opened_at_ms) < breaker->cfg.open_duration_ms) {
            return ZV_ERR_CIRCUIT_OPEN;
        }
        breaker->state = ZV_CB_HALF_OPEN;
        breaker->half_open_trials = 0u;
    }
    if (breaker->half_open_trials < breaker->cfg.half_open_max_trials) {
        breaker->half_open_trials++;
        return ZV_OK;
    }
    return ZV_ERR_CIRCUIT_OPEN;
}

void zv_cb_record_success(zv_circuit_breaker_t *breaker)
{
    if (breaker == NULL) {
        return;
    }
    breaker->state = ZV_CB_CLOSED;
    breaker->consecutive_failures = 0u;
    breaker->half_open_trials = 0u;
}

void zv_cb_record_failure(zv_circuit_breaker_t *breaker, uint32_t now_ms)
{
    if (breaker == NULL) {
        return;
    }
    if (breaker->consecutive_failures < UINT32_MAX) {
        breaker->consecutive_failures++;
    }
    if (breaker->state == ZV_CB_HALF_OPEN || breaker->consecutive_failures >= breaker->cfg.failure_threshold) {
        breaker->state = ZV_CB_OPEN;
        breaker->opened_at_ms = now_ms;
        breaker->half_open_trials = 0u;
        breaker->trip_count++;
    }
}

uint32_t zv_cb_remaining_open_ms(const zv_circuit_breaker_t *breaker, uint32_t now_ms)
{
    if (breaker == NULL || breaker->state != ZV_CB_OPEN) {
        return 0u;
    }
    uint32_t elapsed = (uint32_t)(now_ms - breaker->opened_at_ms);
    return elapsed >= breaker->cfg.open_duration_ms ? 0u : breaker->cfg.open_duration_ms - elapsed;
}
