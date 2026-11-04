/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 */
#include "zv_approval.h"

#include <stddef.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static size_t bounded_len(const char *s, size_t cap)
{
    size_t n = 0u;
    while (n < cap && s[n] != '\0') {
        n++;
    }
    return n;
}

static bool is_lower_hex(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

static bool is_b64url(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
}

static bool valid_uuid(const char *s)
{
    if (bounded_len(s, ZV_APPROVAL_ID_LEN + 1u) != ZV_APPROVAL_ID_LEN) {
        return false;
    }
    for (size_t i = 0u; i < ZV_APPROVAL_ID_LEN; i++) {
        bool hyphen_pos = (i == 8u || i == 13u || i == 18u || i == 23u);
        if (hyphen_pos ? (s[i] != '-') : !is_lower_hex(s[i])) {
            return false;
        }
    }
    return true;
}

static bool valid_charset(const char *s, size_t min, size_t max, bool allow_upper)
{
    size_t n = bounded_len(s, max + 1u);
    if (n < min || n > max) {
        return false;
    }
    for (size_t i = 0u; i < n; i++) {
        char c = s[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' ||
                  (allow_upper && c >= 'A' && c <= 'Z');
        if (!ok) {
            return false;
        }
    }
    return true;
}

static bool valid_summary(const char *s)
{
    size_t n = bounded_len(s, ZV_APPROVAL_SUMMARY_MAX + 1u);
    if (n == 0u || n > ZV_APPROVAL_SUMMARY_MAX) {
        return false;
    }
    for (size_t i = 0u; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20u || c == 0x7Fu) {
            return false;
        }
    }
    return true;
}

/*
 * Explicit allowlist rather than a loosened check. Exactly two tiers may ever be
 * signed:
 *   "destructive" - Hermes-originated work with outside blast radius.
 *   "mutating"    - a gateway device command that changes local device state.
 * Anything else still fails closed. The signature canonical string already binds
 * `risk`, so the tier cannot be swapped after signing without breaking the digest.
 */
static bool valid_risk(const char *risk)
{
    size_t n = bounded_len(risk, ZV_APPROVAL_RISK_MAX + 1u);
    if (n == 0u || n > ZV_APPROVAL_RISK_MAX) {
        return false;
    }
    return (n == 11u && memcmp(risk, "destructive", 11u) == 0) ||
           (n == 8u && memcmp(risk, "mutating", 8u) == 0);
}

static zv_status_t validate_structure(const zv_approval_request_t *req)
{
    if (!valid_uuid(req->approval_id) || !valid_charset(req->action, 1u, ZV_APPROVAL_ACTION_MAX, false) ||
        !valid_summary(req->summary) || !valid_charset(req->kid, 1u, ZV_APPROVAL_KID_MAX, true)) {
        return ZV_ERR_INVALID_ARG;
    }
    if (!valid_risk(req->risk)) {
        return ZV_ERR_INVALID_ARG;
    }
    if (bounded_len(req->action_digest, ZV_APPROVAL_DIGEST_LEN + 1u) != ZV_APPROVAL_DIGEST_LEN) {
        return ZV_ERR_INVALID_ARG;
    }
    for (size_t i = 0u; i < ZV_APPROVAL_DIGEST_LEN; i++) {
        if (!is_lower_hex(req->action_digest[i])) {
            return ZV_ERR_INVALID_ARG;
        }
    }
    size_t nonce_len = bounded_len(req->nonce, ZV_APPROVAL_NONCE_MAX + 1u);
    if (nonce_len < ZV_APPROVAL_NONCE_MIN || nonce_len > ZV_APPROVAL_NONCE_MAX) {
        return ZV_ERR_INVALID_ARG;
    }
    for (size_t i = 0u; i < nonce_len; i++) {
        if (!is_b64url(req->nonce[i])) {
            return ZV_ERR_INVALID_ARG;
        }
    }
    if (req->issued_at_s <= 0 || req->expires_at_s <= req->issued_at_s) {
        return ZV_ERR_INVALID_ARG;
    }
    return ZV_OK;
}

zv_approval_policy_t zv_approval_default_policy(void)
{
    zv_approval_policy_t policy = {.max_ttl_s = 60u, .max_clock_skew_s = 5u};
    return policy;
}

zv_status_t zv_approval_validate(const zv_approval_request_t *req, int64_t now_s,
                                 const zv_approval_policy_t *policy)
{
    if (req == NULL || policy == NULL || policy->max_ttl_s == 0u) {
        return ZV_ERR_INVALID_ARG;
    }
    zv_status_t status = validate_structure(req);
    if (status != ZV_OK) {
        return status;
    }
    if (req->expires_at_s - req->issued_at_s > (int64_t)policy->max_ttl_s) {
        return ZV_ERR_POLICY;
    }
    if (req->issued_at_s > now_s + (int64_t)policy->max_clock_skew_s) {
        return ZV_ERR_NOT_YET_VALID;
    }
    if (now_s >= req->expires_at_s) {
        return ZV_ERR_EXPIRED;
    }
    return ZV_OK;
}

zv_status_t zv_approval_canonical(const zv_approval_request_t *req, char *buf, size_t buf_size, size_t *len)
{
    if (req == NULL || buf == NULL || buf_size == 0u || len == NULL) {
        return ZV_ERR_INVALID_ARG;
    }
    zv_status_t status = validate_structure(req);
    if (status != ZV_OK) {
        return status;
    }
    int n = snprintf(buf, buf_size, "%s\n%s\n%s\n%s\n%s\n%s\n%" PRId64 "\n%" PRId64 "\n%s\n%s",
                     ZV_APPROVAL_CANONICAL_PREFIX, req->approval_id, req->action, req->risk, req->action_digest,
                     req->nonce, req->issued_at_s, req->expires_at_s, req->kid, req->summary);
    if (n < 0) {
        return ZV_ERR_INVALID_ARG;
    }
    if ((size_t)n >= buf_size) {
        return ZV_ERR_BUFFER_TOO_SMALL;
    }
    *len = (size_t)n;
    return ZV_OK;
}

static bool valid_device_id(const char *s)
{
    size_t n = bounded_len(s, ZV_APPROVAL_DEVICE_ID_MAX + 1u);
    if (n < 8u || n > ZV_APPROVAL_DEVICE_ID_MAX) {
        return false;
    }
    for (size_t i = 0u; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) {
            return false;
        }
    }
    return true;
}

static bool valid_signature(const char *signature, size_t signature_len)
{
    if (signature_len < ZV_APPROVAL_SIG_MIN || signature_len > ZV_APPROVAL_SIG_MAX) {
        return false;
    }
    /* Reject an embedded NUL so the verifier never sees a shorter secret string. */
    for (size_t i = 0u; i < signature_len; i++) {
        if (signature[i] == '\0' || !is_b64url(signature[i])) {
            return false;
        }
    }
    return true;
}

const char *zv_approval_decision_name(zv_approval_outcome_t outcome)
{
    switch (outcome) {
    case ZV_APPROVAL_OUTCOME_APPROVED:
        return "approved";
    case ZV_APPROVAL_OUTCOME_DENIED:
        return "denied";
    case ZV_APPROVAL_OUTCOME_EXPIRED:
        return "expired";
    }
    return "unknown";
}

bool zv_approval_decision_from_hold(zv_hold_state_t state, zv_approval_outcome_t *outcome)
{
    if (outcome == NULL || !zv_hold_is_terminal(state)) {
        return false;
    }
    switch (state) {
    case ZV_HOLD_CONFIRMED:
        *outcome = ZV_APPROVAL_OUTCOME_APPROVED;
        return true;
    case ZV_HOLD_DENIED:
        *outcome = ZV_APPROVAL_OUTCOME_DENIED;
        return true;
    case ZV_HOLD_EXPIRED:
        *outcome = ZV_APPROVAL_OUTCOME_EXPIRED;
        return true;
    default:
        *outcome = ZV_APPROVAL_OUTCOME_DENIED;
        return false;
    }
}

zv_status_t zv_approval_verify(const zv_approval_request_t *req, const char *signature, size_t signature_len,
                               int64_t now_s, const zv_approval_policy_t *policy, const zv_approval_gate_t *gate)
{
    if (req == NULL || signature == NULL || policy == NULL || gate == NULL || gate->trust == NULL ||
        gate->nonce_cache == NULL || gate->verify == NULL) {
        return ZV_ERR_INVALID_ARG;
    }
    if (!valid_signature(signature, signature_len)) {
        return ZV_ERR_INVALID_ARG;
    }
    /* Structure and time before any crypto: cheap rejections never touch a key. */
    zv_status_t status = zv_approval_validate(req, now_s, policy);
    if (status != ZV_OK) {
        return status;
    }
    const zv_trust_key_t *key = zv_trust_store_find(gate->trust, req->kid, now_s);
    if (key == NULL) {
        /* Unknown, retired, or out-of-window kid. Reported as a bad signature so
         * the puck does not confirm whether a kid exists. */
        return ZV_ERR_SIGNATURE;
    }
    char canonical[ZV_APPROVAL_CANONICAL_MAX];
    size_t canonical_len = 0u;
    status = zv_approval_canonical(req, canonical, sizeof(canonical), &canonical_len);
    if (status != ZV_OK) {
        return status;
    }
    if (!gate->verify(key->public_key, key->public_key_len, canonical, canonical_len, signature, signature_len,
                      gate->verify_ctx)) {
        return ZV_ERR_SIGNATURE;
    }
    /* Authenticated at last: only now may this nonce be consumed. */
    return zv_nonce_cache_check_and_insert(gate->nonce_cache, req->nonce, req->expires_at_s, now_s);
}

zv_status_t zv_approval_response_build(char *buf, size_t buf_size, const char *approval_id,
                                       zv_approval_outcome_t outcome, const char *device_id, int64_t decided_at_s)
{
    if (buf == NULL || buf_size == 0u || approval_id == NULL || device_id == NULL || decided_at_s <= 0) {
        return ZV_ERR_INVALID_ARG;
    }
    if (!valid_uuid(approval_id) || !valid_device_id(device_id)) {
        return ZV_ERR_INVALID_ARG;
    }
    const char *decision = zv_approval_decision_name(outcome);
    if (strcmp(decision, "unknown") == 0) {
        return ZV_ERR_INVALID_ARG;
    }
    int n = snprintf(buf, buf_size,
                     "{\"v\":1,\"type\":\"approval.response\",\"approval_id\":\"%s\",\"decision\":\"%s\","
                     "\"device_id\":\"%s\",\"decided_at\":%" PRId64 "}",
                     approval_id, decision, device_id, decided_at_s);
    if (n < 0) {
        return ZV_ERR_INVALID_ARG;
    }
    if ((size_t)n >= buf_size) {
        return ZV_ERR_BUFFER_TOO_SMALL;
    }
    return ZV_OK;
}

void zv_nonce_cache_init(zv_nonce_cache_t *cache)
{
    if (cache != NULL) {
        memset(cache, 0, sizeof(*cache));
    }
}

zv_status_t zv_nonce_cache_check_and_insert(zv_nonce_cache_t *cache, const char *nonce, int64_t expires_at_s,
                                            int64_t now_s)
{
    if (cache == NULL || nonce == NULL) {
        return ZV_ERR_INVALID_ARG;
    }
    size_t n = bounded_len(nonce, ZV_APPROVAL_NONCE_MAX + 1u);
    if (n < ZV_APPROVAL_NONCE_MIN || n > ZV_APPROVAL_NONCE_MAX) {
        return ZV_ERR_INVALID_ARG;
    }
    zv_nonce_entry_t *free_slot = NULL;
    for (size_t i = 0u; i < ZV_NONCE_CACHE_SIZE; i++) {
        zv_nonce_entry_t *entry = &cache->entries[i];
        bool live = entry->used && entry->expires_at_s > now_s;
        if (live && strcmp(entry->nonce, nonce) == 0) {
            return ZV_ERR_REPLAY;
        }
        if (!live && free_slot == NULL) {
            free_slot = entry;
        }
    }
    if (free_slot == NULL) {
        return ZV_ERR_QUEUE_FULL;
    }
    memcpy(free_slot->nonce, nonce, n + 1u);
    free_slot->expires_at_s = expires_at_s;
    free_slot->used = true;
    return ZV_OK;
}

bool zv_hold_is_terminal(zv_hold_state_t state)
{
    return state == ZV_HOLD_CONFIRMED || state == ZV_HOLD_DENIED || state == ZV_HOLD_EXPIRED;
}

zv_status_t zv_hold_init(zv_hold_confirm_t *hold, uint32_t now_ms, uint32_t arm_delay_ms,
                         uint32_t hold_required_ms, uint32_t ttl_ms)
{
    if (hold == NULL || hold_required_ms == 0u || ttl_ms <= arm_delay_ms + hold_required_ms) {
        return ZV_ERR_INVALID_ARG;
    }
    hold->state = (arm_delay_ms == 0u) ? ZV_HOLD_READY : ZV_HOLD_ARMING;
    hold->shown_at_ms = now_ms;
    hold->arm_delay_ms = arm_delay_ms;
    hold->hold_required_ms = hold_required_ms;
    hold->ttl_ms = ttl_ms;
    hold->press_started_ms = 0u;
    hold->held_ms = 0u;
    hold->progress_permille = 0u;
    return ZV_OK;
}

zv_hold_state_t zv_hold_tick(zv_hold_confirm_t *hold, uint32_t now_ms)
{
    if (hold == NULL) {
        return ZV_HOLD_EXPIRED;
    }
    if (zv_hold_is_terminal(hold->state)) {
        return hold->state;
    }
    uint32_t shown_for = (uint32_t)(now_ms - hold->shown_at_ms);
    if (hold->state == ZV_HOLD_PRESSING) {
        hold->held_ms = (uint32_t)(now_ms - hold->press_started_ms);
        if (hold->held_ms >= hold->hold_required_ms) {
            hold->progress_permille = 1000u;
            hold->state = ZV_HOLD_CONFIRMED;
            return hold->state;
        }
        hold->progress_permille = (uint16_t)(((uint64_t)hold->held_ms * 1000u) / hold->hold_required_ms);
    }
    if (shown_for >= hold->ttl_ms) {
        hold->progress_permille = 0u;
        hold->state = ZV_HOLD_EXPIRED;
        return hold->state;
    }
    if (hold->state == ZV_HOLD_ARMING && shown_for >= hold->arm_delay_ms) {
        hold->state = ZV_HOLD_READY;
    }
    return hold->state;
}

zv_hold_state_t zv_hold_press(zv_hold_confirm_t *hold, uint32_t now_ms)
{
    zv_hold_state_t state = zv_hold_tick(hold, now_ms);
    if (state == ZV_HOLD_READY) {
        hold->state = ZV_HOLD_PRESSING;
        hold->press_started_ms = now_ms;
        hold->held_ms = 0u;
        hold->progress_permille = 0u;
    }
    return hold == NULL ? ZV_HOLD_EXPIRED : hold->state;
}

zv_hold_state_t zv_hold_release(zv_hold_confirm_t *hold, uint32_t now_ms)
{
    zv_hold_state_t state = zv_hold_tick(hold, now_ms);
    if (state == ZV_HOLD_PRESSING) {
        hold->state = ZV_HOLD_READY;
        hold->held_ms = 0u;
        hold->progress_permille = 0u;
    }
    return hold == NULL ? ZV_HOLD_EXPIRED : hold->state;
}

zv_hold_state_t zv_hold_deny(zv_hold_confirm_t *hold, uint32_t now_ms)
{
    zv_hold_state_t state = zv_hold_tick(hold, now_ms);
    if (hold != NULL && !zv_hold_is_terminal(state)) {
        hold->state = ZV_HOLD_DENIED;
        hold->progress_permille = 0u;
    }
    return hold == NULL ? ZV_HOLD_EXPIRED : hold->state;
}
