/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Status codes shared by the portable core (host-testable) and the firmware
 * components. Firmware code converts esp_err_t at the component boundary;
 * core code never returns a silent success on invalid input.
 */
typedef enum {
    ZV_OK = 0,
    ZV_ERR_INVALID_ARG,
    ZV_ERR_INVALID_STATE,
    ZV_ERR_INVALID_TRANSITION,
    ZV_ERR_QUEUE_FULL,
    ZV_ERR_QUEUE_EMPTY,
    ZV_ERR_BUFFER_TOO_SMALL,
    ZV_ERR_EXPIRED,
    ZV_ERR_NOT_YET_VALID,
    ZV_ERR_REPLAY,
    ZV_ERR_SIGNATURE,
    ZV_ERR_POLICY,
    ZV_ERR_CIRCUIT_OPEN,
    ZV_ERR_UNSUPPORTED,
    ZV_ERR_HARDWARE_GATE,
    ZV_ERR_COUNT
} zv_status_t;

/** Stable name for a status code. Never returns NULL. */
const char *zv_status_str(zv_status_t status);

#ifdef __cplusplus
}
#endif
