/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 */
#include "zv_link_supervisor.h"

#include <stddef.h>

zv_status_t zv_link_init(zv_link_supervisor_t *sup, const zv_link_config_t *cfg, zv_rand_fn_t rand_fn,
                         void *rand_ctx, uint32_t now_ms)
{
    if (sup == NULL || cfg == NULL || cfg->degraded_after_failures == 0u) {
        return ZV_ERR_INVALID_ARG;
    }
    zv_status_t status = zv_backoff_init(&sup->backoff, cfg->backoff_base_ms, cfg->backoff_max_ms, rand_fn, rand_ctx);
    if (status != ZV_OK) {
        return status;
    }
    status = zv_cb_init(&sup->breaker, &cfg->breaker);
    if (status != ZV_OK) {
        return status;
    }
    sup->state = ZV_LINK_OFFLINE;
    sup->wait_start_ms = now_ms;
    sup->wait_ms = 0u;
    sup->consecutive_failures = 0u;
    sup->degraded_after_failures = cfg->degraded_after_failures;
    sup->attempt_in_flight = false;
    return ZV_OK;
}

zv_status_t zv_link_poll(zv_link_supervisor_t *sup, uint32_t now_ms, bool *should_attempt)
{
    if (sup == NULL || should_attempt == NULL) {
        return ZV_ERR_INVALID_ARG;
    }
    *should_attempt = false;
    if (sup->attempt_in_flight || sup->state == ZV_LINK_ONLINE) {
        return ZV_OK;
    }
    if ((uint32_t)(now_ms - sup->wait_start_ms) < sup->wait_ms) {
        return ZV_OK;
    }
    if (zv_cb_allow(&sup->breaker, now_ms) != ZV_OK) {
        /* Breaker is open: stay degraded and wait out the open window. */
        sup->state = ZV_LINK_DEGRADED;
        return ZV_OK;
    }
    sup->attempt_in_flight = true;
    if (sup->state != ZV_LINK_DEGRADED && sup->state != ZV_LINK_RECONNECTING) {
        sup->state = ZV_LINK_CONNECTING;
    }
    *should_attempt = true;
    return ZV_OK;
}

zv_status_t zv_link_on_connected(zv_link_supervisor_t *sup)
{
    if (sup == NULL) {
        return ZV_ERR_INVALID_ARG;
    }
    sup->attempt_in_flight = false;
    sup->consecutive_failures = 0u;
    sup->wait_ms = 0u;
    zv_backoff_reset(&sup->backoff);
    zv_cb_record_success(&sup->breaker);
    sup->state = ZV_LINK_ONLINE;
    return ZV_OK;
}

zv_status_t zv_link_on_reconnect(zv_link_supervisor_t *sup, uint32_t now_ms)
{
    if (sup == NULL) return ZV_ERR_INVALID_ARG;
    if (sup->state != ZV_LINK_ONLINE) return ZV_ERR_INVALID_STATE;
    sup->attempt_in_flight = false;
    sup->wait_start_ms = now_ms;
    sup->wait_ms = 0u;
    sup->state = ZV_LINK_RECONNECTING;
    return ZV_OK;
}

zv_status_t zv_link_on_failure(zv_link_supervisor_t *sup, uint32_t now_ms)
{
    if (sup == NULL) {
        return ZV_ERR_INVALID_ARG;
    }
    uint32_t delay_ms = 0u;
    zv_status_t status = zv_backoff_next(&sup->backoff, &delay_ms);
    if (status != ZV_OK) {
        return status;
    }
    sup->attempt_in_flight = false;
    if (sup->consecutive_failures < UINT32_MAX) {
        sup->consecutive_failures++;
    }
    zv_cb_record_failure(&sup->breaker, now_ms);

    uint32_t open_ms = zv_cb_remaining_open_ms(&sup->breaker, now_ms);
    sup->wait_start_ms = now_ms;
    sup->wait_ms = (open_ms > delay_ms) ? open_ms : delay_ms;

    if (sup->breaker.state == ZV_CB_OPEN || sup->consecutive_failures >= sup->degraded_after_failures) {
        sup->state = ZV_LINK_DEGRADED;
    } else {
        sup->state = ZV_LINK_OFFLINE;
    }
    return ZV_OK;
}

zv_link_state_t zv_link_state(const zv_link_supervisor_t *sup)
{
    return sup == NULL ? ZV_LINK_OFFLINE : sup->state;
}

const char *zv_link_state_name(zv_link_state_t state)
{
    switch (state) {
    case ZV_LINK_OFFLINE:
        return "offline";
    case ZV_LINK_CONNECTING:
        return "connecting";
    case ZV_LINK_ONLINE:
        return "online";
    case ZV_LINK_DEGRADED:
        return "degraded";
    case ZV_LINK_RECONNECTING:
        return "cancelled";
    }
    return "unknown";
}
