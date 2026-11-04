/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 *
 * M3 Hermes approval: the concrete ES256 verifier behind zv_core's portable
 * approval gate, plus a boot known-answer self-test.
 *
 * zv_core stays free of ESP-IDF and mbedtls; this component supplies the
 * platform crypto and wires it into zv_approval_gate_t.verify.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "zv_approval.h"
#include "zv_trust_store.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * ES256 (ECDSA P-256 over SHA-256) verification, matching the signature scheme
 * in protocol/v1/test-vectors/approval-signature.json.
 *
 * `public_key` is a SEC1 uncompressed point (0x04 || X || Y). `signature` is
 * base64url text whose decoded value is the raw r||s pair, exactly as the
 * vector generator emits it. Returns false on any malformed input, an
 * off-curve point, or a signature that does not verify; it never reports a
 * failure as a success.
 *
 * Signature directly as a zv_approval_gate_t.verify implementation.
 */
bool zv_hermes_es256_verify(const uint8_t *public_key, size_t public_key_len, const char *canonical,
                            size_t canonical_len, const char *signature, size_t signature_len, void *ctx);

/** Builds a trust key from a base64url SEC1 uncompressed public key. */
esp_err_t zv_hermes_key_from_b64(const char *kid, const char *public_key_sec1_b64url, zv_trust_key_t *out);

/**
 * Runs the full ordered approval gate against a caller-supplied trust store and
 * nonce cache, with the real mbedtls verifier bound in. now_s is supplied by
 * the caller rather than read from a clock inside this component, so the SNTP
 * and RTC paths stay the owner's decision.
 */
zv_status_t zv_hermes_check(const zv_approval_request_t *req, const char *signature, size_t signature_len,
                            int64_t now_s, const zv_approval_policy_t *policy, const zv_trust_store_t *trust,
                            zv_nonce_cache_t *nonce_cache);

/**
 * Boot known-answer test against the committed TEST-ONLY vector.
 *
 * Proves the whole M3 path on real hardware: a genuine request verifies, a
 * tampered summary fails the signature check, a tampered signature fails, a
 * replay is refused, an unknown kid is refused, and an expired request is
 * refused. Returns ESP_OK only when every one of those behaved as specified.
 *
 * The embedded vector is throwaway test material and must never be trusted by a
 * production gateway or device.
 */
esp_err_t zv_hermes_selftest(void);

/** SHA-256 adapter for the portable device command gate. */
bool zv_hermes_command_digest_verify(const char *canonical, size_t canonical_len, const char *expected_hex, void *ctx);

#ifdef __cplusplus
}
#endif