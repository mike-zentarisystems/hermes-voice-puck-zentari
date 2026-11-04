/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 */
#include "zv_cred_store.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static const char *const k_scope_names[ZV_SCOPE_COUNT] = {
    [ZV_SCOPE_SESSION_CONNECT] = "session:connect",
    [ZV_SCOPE_AUDIO_STREAM] = "audio:stream",
    [ZV_SCOPE_EVENTS_RECEIVE] = "events:receive",
    [ZV_SCOPE_APPROVAL_RESPOND] = "approval:respond",
    [ZV_SCOPE_OTA_READ] = "ota:read",
};

zv_status_t zv_cred_scope_from_name(const char *name, zv_cred_scope_t *out)
{
    if (name == NULL || out == NULL) {
        return ZV_ERR_INVALID_ARG;
    }
    for (int i = 0; i < (int)ZV_SCOPE_COUNT; i++) {
        if (strcmp(name, k_scope_names[i]) == 0) {
            *out = (zv_cred_scope_t)i;
            return ZV_OK;
        }
    }
    return ZV_ERR_POLICY;
}

const char *zv_cred_scope_name(zv_cred_scope_t scope)
{
    if ((unsigned)scope >= (unsigned)ZV_SCOPE_COUNT) {
        return "unknown";
    }
    return k_scope_names[scope];
}

static bool device_id_ok(const char *id)
{
    size_t n = strlen(id);
    if (n < ZV_CRED_DEVICE_ID_MIN || n > ZV_CRED_DEVICE_ID_MAX) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) {
            return false;
        }
    }
    return true;
}

zv_status_t zv_cred_validate(const zv_device_credential_t *cred)
{
    if (cred == NULL) {
        return ZV_ERR_INVALID_ARG;
    }
    if (!device_id_ok(cred->device_id)) {
        return ZV_ERR_POLICY;
    }
    if (strncmp(cred->gateway_origin, "wss://", 6) != 0 || cred->gateway_origin[6] == '\0') {
        return ZV_ERR_POLICY;
    }
    if (cred->secret_len < ZV_CRED_SECRET_MIN || cred->secret_len > ZV_CRED_SECRET_MAX) {
        return ZV_ERR_POLICY;
    }
    if (cred->secret[cred->secret_len] != '\0') {
        return ZV_ERR_INVALID_STATE;
    }
    if (cred->scope_mask == 0u || (cred->scope_mask & ~((1u << ZV_SCOPE_COUNT) - 1u)) != 0u) {
        return ZV_ERR_POLICY;
    }
    if (cred->generation < 1u) {
        return ZV_ERR_POLICY;
    }
    if (cred->expires_at_s <= 0) {
        return ZV_ERR_POLICY;
    }
    return ZV_OK;
}

zv_status_t zv_cred_format_public(const zv_device_credential_t *cred, char *buf, size_t buf_size)
{
    if (cred == NULL || buf == NULL || buf_size == 0u) {
        return ZV_ERR_INVALID_ARG;
    }
    int n = snprintf(buf, buf_size, "device=%s origin=%s gen=%" PRIu32 " scopes=0x%" PRIx32 " secret_len=%u REDACTED",
                     cred->device_id, cred->gateway_origin, cred->generation, cred->scope_mask,
                     (unsigned)cred->secret_len);
    if (n <= 0 || (size_t)n >= buf_size) {
        return ZV_ERR_BUFFER_TOO_SMALL;
    }
    return ZV_OK;
}

void zv_cred_store_reset(zv_cred_store_t *store)
{
    if (store == NULL) {
        return;
    }
    memset(store, 0, sizeof(*store));
}

zv_status_t zv_cred_store_install_active(zv_cred_store_t *store, const zv_device_credential_t *cred)
{
    if (store == NULL || cred == NULL) {
        return ZV_ERR_INVALID_ARG;
    }
    if (zv_cred_validate(cred) != ZV_OK) {
        return ZV_ERR_POLICY;
    }
    if (store->has_active && cred->generation <= store->active.generation) {
        return ZV_ERR_POLICY;
    }
    store->active = *cred;
    store->has_active = true;
    memset(&store->staged, 0, sizeof(store->staged));
    store->has_staged = false;
    return ZV_OK;
}

zv_status_t zv_cred_store_stage(zv_cred_store_t *store, const zv_device_credential_t *cred)
{
    if (store == NULL || cred == NULL) {
        return ZV_ERR_INVALID_ARG;
    }
    if (zv_cred_validate(cred) != ZV_OK) {
        return ZV_ERR_POLICY;
    }
    if (!store->has_active) {
        return ZV_ERR_INVALID_STATE;
    }
    if (store->has_staged) {
        return ZV_ERR_INVALID_STATE;
    }
    if (cred->generation <= store->active.generation) {
        return ZV_ERR_POLICY;
    }
    if (strcmp(cred->device_id, store->active.device_id) != 0) {
        return ZV_ERR_POLICY;
    }
    if (strcmp(cred->gateway_origin, store->active.gateway_origin) != 0) {
        return ZV_ERR_POLICY;
    }
    store->staged = *cred;
    store->has_staged = true;
    return ZV_OK;
}

zv_status_t zv_cred_store_commit_staged(zv_cred_store_t *store)
{
    if (store == NULL) {
        return ZV_ERR_INVALID_ARG;
    }
    if (!store->has_staged) {
        return ZV_ERR_INVALID_STATE;
    }
    store->active = store->staged;
    store->has_active = true;
    memset(&store->staged, 0, sizeof(store->staged));
    store->has_staged = false;
    return ZV_OK;
}

zv_status_t zv_cred_store_drop_staged(zv_cred_store_t *store)
{
    if (store == NULL) {
        return ZV_ERR_INVALID_ARG;
    }
    if (!store->has_staged) {
        return ZV_ERR_INVALID_STATE;
    }
    memset(&store->staged, 0, sizeof(store->staged));
    store->has_staged = false;
    return ZV_OK;
}

const zv_device_credential_t *zv_cred_store_current(const zv_cred_store_t *store)
{
    if (store == NULL) {
        return NULL;
    }
    if (store->has_staged) {
        return &store->staged;
    }
    if (store->has_active) {
        return &store->active;
    }
    return NULL;
}
