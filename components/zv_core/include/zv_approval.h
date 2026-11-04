/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "zv_event.h"
#include "zv_status.h"
#include "zv_trust_store.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZV_APPROVAL_ID_LEN 36u         /**< Canonical UUID text length */
#define ZV_APPROVAL_ACTION_MAX 47u     /**< [a-z0-9._-]{1,47} */
#define ZV_APPROVAL_RISK_MAX 15u
#define ZV_APPROVAL_DIGEST_LEN 64u     /**< Lower-case hex SHA-256 of the canonical action */
#define ZV_APPROVAL_NONCE_MIN 22u      /**< base64url, >= 128 bits */
#define ZV_APPROVAL_NONCE_MAX 48u
#define ZV_APPROVAL_KID_MAX 32u
#define ZV_APPROVAL_SUMMARY_MAX 159u   /**< UTF-8 bytes, no control characters */
#define ZV_APPROVAL_CANONICAL_MAX 512u
#define ZV_APPROVAL_SIG_MIN 80u /**< protocol/v1 approval.request signature pattern */
#define ZV_APPROVAL_SIG_MAX 512u
#define ZV_APPROVAL_DEVICE_ID_MAX 64u
#define ZV_APPROVAL_RESPONSE_MAX 256u
#define ZV_APPROVAL_CANONICAL_PREFIX "zv-approval-v1"

/** Parsed approval.request payload (see protocol/v1/schemas/approval.request.schema.json). */
typedef struct {
    char approval_id[ZV_APPROVAL_ID_LEN + 1u];
    char action[ZV_APPROVAL_ACTION_MAX + 1u];
    char risk[ZV_APPROVAL_RISK_MAX + 1u];
    char summary[ZV_APPROVAL_SUMMARY_MAX + 1u];
    char action_digest[ZV_APPROVAL_DIGEST_LEN + 1u];
    char nonce[ZV_APPROVAL_NONCE_MAX + 1u];
    char kid[ZV_APPROVAL_KID_MAX + 1u];
    int64_t issued_at_s;
    int64_t expires_at_s;
} zv_approval_request_t;

typedef struct {
    uint32_t max_ttl_s;        /**< Upper bound on expires_at - issued_at */
    uint32_t max_clock_skew_s; /**< Tolerance for issued_at in the future */
} zv_approval_policy_t;

zv_approval_policy_t zv_approval_default_policy(void);

/**
 * Structural and temporal validation. Must pass before signature
 * verification. Expiry has no leeway (fail closed).
 */
zv_status_t zv_approval_validate(const zv_approval_request_t *req, int64_t now_s,
                                 const zv_approval_policy_t *policy);

/** Builds the exact byte string the gateway signed (newline-joined, no trailing newline). */
zv_status_t zv_approval_canonical(const zv_approval_request_t *req, char *buf, size_t buf_size, size_t *len);

/** Replay protection for approval nonces. */
#define ZV_NONCE_CACHE_SIZE 16u
typedef struct {
    char nonce[ZV_APPROVAL_NONCE_MAX + 1u];
    int64_t expires_at_s;
    bool used;
} zv_nonce_entry_t;

typedef struct {
    zv_nonce_entry_t entries[ZV_NONCE_CACHE_SIZE];
} zv_nonce_cache_t;

void zv_nonce_cache_init(zv_nonce_cache_t *cache);

/**
 * Call only after the signature is verified. Returns ZV_ERR_REPLAY for a live
 * duplicate. When every slot holds an unexpired nonce the request is refused
 * with ZV_ERR_QUEUE_FULL (fail closed) instead of evicting a live nonce, which
 * would reopen a replay window.
 */
zv_status_t zv_nonce_cache_check_and_insert(zv_nonce_cache_t *cache, const char *nonce, int64_t expires_at_s,
                                            int64_t now_s);

/**
 * Platform-supplied signature check over the canonical string, so zv_core stays
 * free of ESP-IDF and mbedtls. Must return true only when `signature` is a
 * valid signature by `public_key` over exactly `canonical_len` bytes.
 */
typedef bool (*zv_approval_sig_verify_fn)(const uint8_t *public_key, size_t public_key_len, const char *canonical,
                                          size_t canonical_len, const char *signature, size_t signature_len,
                                          void *ctx);

/** Everything the approval gate needs, so the call site stays a single call. */
typedef struct {
    const zv_trust_store_t *trust;   /**< gateway approval keys, public material only */
    zv_nonce_cache_t *nonce_cache;   /**< mutated on success to block replay */
    zv_approval_sig_verify_fn verify; /**< platform crypto */
    void *verify_ctx;                 /**< passed through to verify, may be NULL */
} zv_approval_gate_t;

/**
 * Full fail-closed gate for one approval.request, in a fixed order:
 * signature format, structural and temporal validation, trusted key lookup,
 * signature verification, and only then nonce insertion.
 *
 * The nonce is reserved last on purpose. An unauthenticated peer that replays or
 * forges a request therefore cannot consume nonce slots and lock out a genuine
 * approval by filling the cache.
 *
 * Returns ZV_OK only when the request may be shown to the operator. Any failure
 * returns ZV_ERR_INVALID_ARG, ZV_ERR_EXPIRED, ZV_ERR_NOT_YET_VALID,
 * ZV_ERR_POLICY, ZV_ERR_SIGNATURE, ZV_ERR_REPLAY, ZV_ERR_BUFFER_TOO_SMALL or
 * ZV_ERR_QUEUE_FULL, and leaves the nonce cache untouched.
 */
zv_status_t zv_approval_verify(const zv_approval_request_t *req, const char *signature, size_t signature_len,
                               int64_t now_s, const zv_approval_policy_t *policy, const zv_approval_gate_t *gate);

/** Wire decision string for an approval outcome. Never returns NULL. */
const char *zv_approval_decision_name(zv_approval_outcome_t outcome);

/**
 * Builds an approval.response frame matching
 * protocol/v1/schemas/approval.response.schema.json. Rejects a non-UUID
 * approval_id, a device_id outside ^[a-z0-9-]{8,64}$, or a non-positive
 * decided_at. Returns ZV_ERR_BUFFER_TOO_SMALL when the frame does not fit.
 */
zv_status_t zv_approval_response_build(char *buf, size_t buf_size, const char *approval_id,
                                       zv_approval_outcome_t outcome, const char *device_id, int64_t decided_at_s);

/** Hold-to-confirm interaction for destructive approvals. */
typedef enum {
    ZV_HOLD_ARMING = 0, /**< Inputs ignored to absorb carry-over taps */
    ZV_HOLD_READY,
    ZV_HOLD_PRESSING,
    ZV_HOLD_CONFIRMED,
    ZV_HOLD_DENIED,
    ZV_HOLD_EXPIRED,
} zv_hold_state_t;

typedef struct {
    zv_hold_state_t state;
    uint32_t shown_at_ms;
    uint32_t arm_delay_ms;
    uint32_t hold_required_ms;
    uint32_t ttl_ms;
    uint32_t press_started_ms;
    uint32_t held_ms;
    uint16_t progress_permille;
} zv_hold_confirm_t;

#define ZV_HOLD_DEFAULT_ARM_DELAY_MS 400u
#define ZV_HOLD_DEFAULT_REQUIRED_MS 1500u

zv_status_t zv_hold_init(zv_hold_confirm_t *hold, uint32_t now_ms, uint32_t arm_delay_ms,
                         uint32_t hold_required_ms, uint32_t ttl_ms);
zv_hold_state_t zv_hold_press(zv_hold_confirm_t *hold, uint32_t now_ms);
zv_hold_state_t zv_hold_release(zv_hold_confirm_t *hold, uint32_t now_ms);
zv_hold_state_t zv_hold_deny(zv_hold_confirm_t *hold, uint32_t now_ms);
zv_hold_state_t zv_hold_tick(zv_hold_confirm_t *hold, uint32_t now_ms);
bool zv_hold_is_terminal(zv_hold_state_t state);

/**
 * Maps a terminal hold state to the decision sent to the gateway. Returns false
 * for any non-terminal state so a partial hold can never produce a response.
 */
bool zv_approval_decision_from_hold(zv_hold_state_t state, zv_approval_outcome_t *outcome);

#ifdef __cplusplus
}
#endif
