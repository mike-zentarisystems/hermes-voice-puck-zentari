/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 *
 * Bounded M2 provisioning acceptance: physical presence, encrypted store,
 * rotation policy. Default OFF. Platform ready stays false.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * After the M0 display challenge, requires a continuous touch hold, then
 * installs a non-secret fixture credential into encrypted NVS, reloads it,
 * exercises staged rotation (including rollback of a lower generation), and
 * erases the namespace. Never logs credential secret material.
 */
esp_err_t zv_provision_acceptance_demo(void);

#ifdef __cplusplus
}
#endif
