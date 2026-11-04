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

typedef enum {
    ZV_CB_CLOSED = 0,
    ZV_CB_OPEN,
    ZV_CB_HALF_OPEN,
} zv_cb_state_t;

typedef struct {
    uint32_t failure_threshold;    /**< Consecutive failures that trip the breaker */
    uint32_t open_duration_ms;     /**< Time the breaker stays open before probing */
    uint32_t half_open_max_trials; /**< Probe attempts allowed while half-open */
} zv_cb_config_t;

/**
 * Circuit breaker for gateway connection attempts. All time arithmetic uses
 * unsigned subtraction so millisecond counter wrap-around is handled.
 */
typedef struct {
    zv_cb_config_t cfg;
    zv_cb_state_t state;
    uint32_t consecutive_failures;
    uint32_t opened_at_ms;
    uint32_t half_open_trials;
    uint32_t trip_count;
} zv_circuit_breaker_t;

zv_status_t zv_cb_init(zv_circuit_breaker_t *breaker, const zv_cb_config_t *cfg);

/** Returns ZV_OK when an attempt may proceed, ZV_ERR_CIRCUIT_OPEN otherwise. */
zv_status_t zv_cb_allow(zv_circuit_breaker_t *breaker, uint32_t now_ms);
void zv_cb_record_success(zv_circuit_breaker_t *breaker);
void zv_cb_record_failure(zv_circuit_breaker_t *breaker, uint32_t now_ms);

/** Milliseconds until an open breaker allows a probe; 0 when not open. */
uint32_t zv_cb_remaining_open_ms(const zv_circuit_breaker_t *breaker, uint32_t now_ms);

#ifdef __cplusplus
}
#endif
