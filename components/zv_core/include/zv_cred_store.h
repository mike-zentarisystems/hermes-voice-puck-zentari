/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 *
 * Portable policy for one scoped gateway device credential.
 * Matches protocol/v1 device.credential. Never logs the secret.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "zv_status.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZV_CRED_DEVICE_ID_MIN 8u
#define ZV_CRED_DEVICE_ID_MAX 64u
#define ZV_CRED_SECRET_MIN 32u
#define ZV_CRED_SECRET_MAX 512u
#define ZV_CRED_MAX_SCOPES 8u
#define ZV_CRED_ORIGIN_MAX 128u

typedef enum {
    ZV_SCOPE_SESSION_CONNECT = 0,
    ZV_SCOPE_AUDIO_STREAM,
    ZV_SCOPE_EVENTS_RECEIVE,
    ZV_SCOPE_APPROVAL_RESPOND,
    ZV_SCOPE_OTA_READ,
    ZV_SCOPE_COUNT
} zv_cred_scope_t;

/** Parsed device.credential. Secret is opaque bytes; never logged. */
typedef struct {
    char device_id[ZV_CRED_DEVICE_ID_MAX + 1u];
    char gateway_origin[ZV_CRED_ORIGIN_MAX + 1u];
    char secret[ZV_CRED_SECRET_MAX + 1u];
    size_t secret_len;
    uint32_t scope_mask;
    uint32_t generation;
    int64_t expires_at_s;
} zv_device_credential_t;

/** Persistent store view: at most one active and one staged credential. */
typedef struct {
    zv_device_credential_t active;
    zv_device_credential_t staged;
    bool has_active;
    bool has_staged;
} zv_cred_store_t;

zv_status_t zv_cred_scope_from_name(const char *name, zv_cred_scope_t *out);
const char *zv_cred_scope_name(zv_cred_scope_t scope);

/**
 * Validates a credential against protocol/v1 device.credential rules:
 * non-empty device_id and wss:// origin, secret length, at least one
 * scope, generation >= 1, expires_at > 0.
 */
zv_status_t zv_cred_validate(const zv_device_credential_t *cred);

/** Writes a redacted one-line summary. Never includes the secret. */
zv_status_t zv_cred_format_public(const zv_device_credential_t *cred, char *buf, size_t buf_size);

void zv_cred_store_reset(zv_cred_store_t *store);

/**
 * Installs the first active credential. Fails if one already exists with a
 * generation >= the candidate (rollback / replay).
 */
zv_status_t zv_cred_store_install_active(zv_cred_store_t *store, const zv_device_credential_t *cred);

/** Stages a rotation candidate. Generation must be strictly greater than active. */
zv_status_t zv_cred_store_stage(zv_cred_store_t *store, const zv_device_credential_t *cred);

zv_status_t zv_cred_store_commit_staged(zv_cred_store_t *store);
zv_status_t zv_cred_store_drop_staged(zv_cred_store_t *store);

/** Credential to present to the gateway: staged if present, else active. */
const zv_device_credential_t *zv_cred_store_current(const zv_cred_store_t *store);

#ifdef __cplusplus
}
#endif
