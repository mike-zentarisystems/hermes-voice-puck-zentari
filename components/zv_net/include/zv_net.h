/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 *
 * Bounded M2 WSS acceptance: STA join, TLS origin policy, device.hello.
 * Default OFF. Platform ready stays false.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Connects Wi-Fi, opens WSS to the configured origin, sends device.hello. */
esp_err_t zv_wss_acceptance_demo(void);

#ifdef __cplusplus
}
#endif
