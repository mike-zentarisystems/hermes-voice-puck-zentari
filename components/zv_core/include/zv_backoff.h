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

/** Random source; firmware passes esp_random, tests pass a deterministic stub. */
typedef uint32_t (*zv_rand_fn_t)(void *ctx);

/**
 * Exponential backoff with "equal jitter": for attempt n the ceiling is
 * min(max_ms, base_ms * 2^n) and the delay is uniformly distributed in
 * [ceiling/2, ceiling]. The lower bound prevents synchronized hot retry
 * loops across a fleet while the jitter spreads reconnect storms.
 */
typedef struct {
    uint32_t base_ms;
    uint32_t max_ms;
    uint32_t attempt;
    zv_rand_fn_t rand_fn;
    void *rand_ctx;
} zv_backoff_t;

zv_status_t zv_backoff_init(zv_backoff_t *backoff, uint32_t base_ms, uint32_t max_ms, zv_rand_fn_t rand_fn,
                            void *rand_ctx);
zv_status_t zv_backoff_next(zv_backoff_t *backoff, uint32_t *delay_ms);
void zv_backoff_reset(zv_backoff_t *backoff);

#ifdef __cplusplus
}
#endif
