/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 */
#include "zv_trust_store.h"

#include <string.h>

static size_t bounded_len(const char *s, size_t cap)
{
    size_t n = 0u;
    while (n < cap && s[n] != '\0') {
        n++;
    }
    return n;
}

static bool valid_kid(const char *kid)
{
    if (kid == NULL) {
        return false;
    }
    size_t n = bounded_len(kid, ZV_TRUST_KEY_ID_MAX + 1u);
    if (n == 0u || n > ZV_TRUST_KEY_ID_MAX) {
        return false;
    }
    for (size_t i = 0u; i < n; i++) {
        char c = kid[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
                  c == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

/** Raw kid lookup that ignores the validity window, so retire always works. */
static zv_trust_key_t *find_raw(zv_trust_store_t *store, const char *kid)
{
    for (size_t i = 0u; i < ZV_TRUST_MAX_KEYS; i++) {
        zv_trust_key_t *entry = &store->keys[i];
        if (entry->used && strcmp(entry->kid, kid) == 0) {
            return entry;
        }
    }
    return NULL;
}

void zv_trust_store_init(zv_trust_store_t *store)
{
    if (store != NULL) {
        memset(store, 0, sizeof(*store));
    }
}

zv_status_t zv_trust_store_add(zv_trust_store_t *store, const zv_trust_key_t *key)
{
    if (store == NULL || key == NULL) {
        return ZV_ERR_INVALID_ARG;
    }
    if (key->curve != ZV_TRUST_CURVE_ES256 || key->public_key_len != ZV_TRUST_P256_KEY_LEN ||
        key->public_key[0] != 0x04u) {
        return ZV_ERR_INVALID_ARG;
    }
    if (!valid_kid(key->kid)) {
        return ZV_ERR_INVALID_ARG;
    }
    if (key->not_after_s != 0 && key->not_after_s <= key->not_before_s) {
        return ZV_ERR_INVALID_ARG;
    }
    if (find_raw(store, key->kid) != NULL) {
        return ZV_ERR_INVALID_STATE;
    }
    for (size_t i = 0u; i < ZV_TRUST_MAX_KEYS; i++) {
        if (!store->keys[i].used) {
            store->keys[i] = *key;
            store->keys[i].used = true;
            return ZV_OK;
        }
    }
    return ZV_ERR_QUEUE_FULL;
}

zv_status_t zv_trust_store_retire(zv_trust_store_t *store, const char *kid)
{
    if (store == NULL || !valid_kid(kid)) {
        return ZV_ERR_INVALID_ARG;
    }
    zv_trust_key_t *entry = find_raw(store, kid);
    if (entry == NULL) {
        return ZV_ERR_INVALID_STATE;
    }
    /* Clear the whole slot so no stale public material outlives the decision. */
    memset(entry, 0, sizeof(*entry));
    return ZV_OK;
}

const zv_trust_key_t *zv_trust_store_find(const zv_trust_store_t *store, const char *kid, int64_t now_s)
{
    if (store == NULL || !valid_kid(kid)) {
        return NULL;
    }
    for (size_t i = 0u; i < ZV_TRUST_MAX_KEYS; i++) {
        const zv_trust_key_t *entry = &store->keys[i];
        if (!entry->used || strcmp(entry->kid, kid) != 0) {
            continue;
        }
        if (now_s < entry->not_before_s) {
            return NULL;
        }
        if (entry->not_after_s != 0 && now_s >= entry->not_after_s) {
            return NULL;
        }
        return entry;
    }
    return NULL;
}

size_t zv_trust_store_count(const zv_trust_store_t *store)
{
    if (store == NULL) {
        return 0u;
    }
    size_t used = 0u;
    for (size_t i = 0u; i < ZV_TRUST_MAX_KEYS; i++) {
        if (store->keys[i].used) {
            used++;
        }
    }
    return used;
}

const char *zv_trust_curve_name(zv_trust_curve_t curve)
{
    switch (curve) {
    case ZV_TRUST_CURVE_ES256:
        return "ES256";
    default:
        return "unknown";
    }
}