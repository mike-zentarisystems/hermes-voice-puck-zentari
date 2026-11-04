/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 *
 * Allowlist of gateway approval verification keys. Public material only: the
 * puck never holds, stores, or transports a gateway private key. This header
 * has no ESP-IDF dependency so it stays host-testable.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "zv_status.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZV_TRUST_KEY_ID_MAX 32u
#define ZV_TRUST_P256_KEY_LEN 65u /**< SEC1 uncompressed point: 0x04 || X(32) || Y(32) */
#define ZV_TRUST_MAX_KEYS 4u

typedef enum {
    ZV_TRUST_CURVE_ES256 = 0, /**< ECDSA P-256 over SHA-256; signature is raw r||s */
} zv_trust_curve_t;

typedef struct {
    char kid[ZV_TRUST_KEY_ID_MAX + 1u];
    zv_trust_curve_t curve;
    uint8_t public_key[ZV_TRUST_P256_KEY_LEN];
    size_t public_key_len;
    int64_t not_before_s; /**< inclusive lower bound */
    int64_t not_after_s;  /**< exclusive upper bound; 0 means "no upper bound" */
    bool used;
} zv_trust_key_t;

typedef struct {
    zv_trust_key_t keys[ZV_TRUST_MAX_KEYS];
} zv_trust_store_t;

void zv_trust_store_init(zv_trust_store_t *store);

/**
 * Installs one verification key.
 *
 * Rejects a malformed kid, an unsupported curve, a key that is not a SEC1
 * uncompressed P-256 point, and an empty validity window, so a malformed entry
 * can never become a trust anchor. Adding the same kid twice is refused rather
 * than silently replacing the key that already signed in-flight approvals.
 */
zv_status_t zv_trust_store_add(zv_trust_store_t *store, const zv_trust_key_t *key);

/** Drops a key so later requests that name it fail closed. */
zv_status_t zv_trust_store_retire(zv_trust_store_t *store, const char *kid);

/**
 * Key eligible to verify a request at now_s, or NULL when the kid is unknown,
 * retired, or outside its validity window. A kid that matches but is outside
 * its window returns NULL rather than falling through to another key.
 *
 * Rotation is expressed as two installed keys whose windows overlap, so an
 * approval signed just before a rollover still verifies while the retired key
 * is drained.
 */
const zv_trust_key_t *zv_trust_store_find(const zv_trust_store_t *store, const char *kid, int64_t now_s);

size_t zv_trust_store_count(const zv_trust_store_t *store);
const char *zv_trust_curve_name(zv_trust_curve_t curve);

#ifdef __cplusplus
}
#endif