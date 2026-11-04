/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 *
 * Encrypted NVS-backed credential persistence for the scoped gateway token.
 * Production still requires flash encryption and Secure Boot V2; NVS
 * encryption alone is the application-layer gate this milestone can prove.
 */
#pragma once

#include "esp_err.h"
#include "zv_cred_store.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Initializes encrypted NVS. Idempotent. */
esp_err_t zv_storage_init(void);

/** Loads active + staged credentials into `store`. Empty flash is not an error. */
esp_err_t zv_storage_load(zv_cred_store_t *store);

/** Persists the store. Secret material is written only to encrypted NVS. */
esp_err_t zv_storage_save(const zv_cred_store_t *store);

/** Erases all credential keys. */
esp_err_t zv_storage_erase(void);

#ifdef __cplusplus
}
#endif
