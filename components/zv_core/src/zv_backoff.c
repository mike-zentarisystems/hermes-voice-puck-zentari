/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 */
#include "zv_backoff.h"

#include <stddef.h>

zv_status_t zv_backoff_init(zv_backoff_t *backoff, uint32_t base_ms, uint32_t max_ms, zv_rand_fn_t rand_fn,
                            void *rand_ctx)
{
    if (backoff == NULL || rand_fn == NULL || base_ms == 0u || max_ms < base_ms) {
        return ZV_ERR_INVALID_ARG;
    }
    backoff->base_ms = base_ms;
    backoff->max_ms = max_ms;
    backoff->attempt = 0u;
    backoff->rand_fn = rand_fn;
    backoff->rand_ctx = rand_ctx;
    return ZV_OK;
}

zv_status_t zv_backoff_next(zv_backoff_t *backoff, uint32_t *delay_ms)
{
    if (backoff == NULL || delay_ms == NULL || backoff->rand_fn == NULL) {
        return ZV_ERR_INVALID_ARG;
    }

    uint32_t ceiling = backoff->max_ms;
    if (backoff->attempt < 32u) {
        uint64_t scaled = (uint64_t)backoff->base_ms << backoff->attempt;
        if (scaled < (uint64_t)ceiling) {
            ceiling = (uint32_t)scaled;
        }
    }
    if (backoff->attempt < UINT32_MAX) {
        backoff->attempt++;
    }

    uint32_t floor_ms = ceiling / 2u;
    uint32_t span = ceiling - floor_ms; /* <= 2^31, so span + 1 cannot overflow */
    uint32_t jitter = backoff->rand_fn(backoff->rand_ctx) % (span + 1u);
    *delay_ms = floor_ms + jitter;
    return ZV_OK;
}

void zv_backoff_reset(zv_backoff_t *backoff)
{
    if (backoff != NULL) {
        backoff->attempt = 0u;
    }
}
