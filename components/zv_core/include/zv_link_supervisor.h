/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "zv_backoff.h"
#include "zv_circuit_breaker.h"
#include "zv_status.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ZV_LINK_OFFLINE = 0, /**< Not connected, retrying normally */
    ZV_LINK_CONNECTING,  /**< Attempt in flight */
    ZV_LINK_ONLINE,      /**< Authenticated session established */
    ZV_LINK_DEGRADED,    /**< Repeated failures or breaker open: local-only UX */
    ZV_LINK_RECONNECTING, /**< Deliberate cancellation; fresh authentication required */
} zv_link_state_t;

typedef struct {
    uint32_t backoff_base_ms;
    uint32_t backoff_max_ms;
    zv_cb_config_t breaker;
    uint32_t degraded_after_failures;
} zv_link_config_t;

/**
 * Combines backoff + circuit breaker into the gateway reconnect policy.
 * The supervisor never performs I/O: the caller polls it, performs the
 * attempt when told to, and reports the outcome.
 */
typedef struct {
    zv_backoff_t backoff;
    zv_circuit_breaker_t breaker;
    zv_link_state_t state;
    uint32_t wait_start_ms;
    uint32_t wait_ms;
    uint32_t consecutive_failures;
    uint32_t degraded_after_failures;
    bool attempt_in_flight;
} zv_link_supervisor_t;

zv_status_t zv_link_init(zv_link_supervisor_t *sup, const zv_link_config_t *cfg, zv_rand_fn_t rand_fn,
                         void *rand_ctx, uint32_t now_ms);

/** Sets *should_attempt when the caller must start a connection attempt now. */
zv_status_t zv_link_poll(zv_link_supervisor_t *sup, uint32_t now_ms, bool *should_attempt);
zv_status_t zv_link_on_connected(zv_link_supervisor_t *sup);

/** Reauthenticate an online session deliberately, without failure/backoff. */
zv_status_t zv_link_on_reconnect(zv_link_supervisor_t *sup, uint32_t now_ms);

/** Report a failed attempt or loss of an established session. */
zv_status_t zv_link_on_failure(zv_link_supervisor_t *sup, uint32_t now_ms);
zv_link_state_t zv_link_state(const zv_link_supervisor_t *sup);
const char *zv_link_state_name(zv_link_state_t state);

#ifdef __cplusplus
}
#endif
