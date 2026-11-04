/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 */
#include "zv_hermes.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "mbedtls/base64.h"
#include "psa/crypto.h"

static const char *TAG = "zv_hermes";

#define ZV_HERMES_DIGEST_LEN 32u
#define ZV_HERMES_SIG_RAW_LEN 64u

/*
 * Decodes base64url into raw bytes. mbedtls_base64_decode only understands the
 * standard alphabet, so '-'/'_' are translated first. Any character that is not
 * valid base64url is rejected outright, so a hostile signature cannot be
 * silently normalised into something that verifies.
 */
static bool decode_b64url(const char *text, size_t text_len, uint8_t *out, size_t out_cap, size_t *out_len)
{
    char b64[ZV_APPROVAL_SIG_MAX + 4u];

    if (text == NULL || out == NULL || out_len == NULL || text_len == 0u || text_len > ZV_APPROVAL_SIG_MAX) {
        return false;
    }
    for (size_t i = 0u; i < text_len; i++) {
        char c = text[i];
        if (c == '-') {
            c = '+';
        } else if (c == '_') {
            c = '/';
        } else if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' ||
                     c == '/' || c == '=')) {
            return false;
        }
        b64[i] = c;
    }
    if (text_len % 4u == 1u) return false;
    size_t padded_len = text_len;
    while (padded_len % 4u != 0u) b64[padded_len++] = '=';
    size_t olen = 0u;
    if (mbedtls_base64_decode(out, out_cap, &olen, (const unsigned char *)b64, padded_len) != 0) {
        return false;
    }
    *out_len = olen;
    return true;
}


bool zv_hermes_es256_verify(const uint8_t *public_key, size_t public_key_len, const char *canonical,
                            size_t canonical_len, const char *signature, size_t signature_len, void *ctx)
{
    (void)ctx;
    if (public_key == NULL || canonical == NULL || signature == NULL || canonical_len == 0u) {
        return false;
    }
    /* The trust store enforces this too, but the verifier must not rely on it. */
    if (public_key_len != ZV_TRUST_P256_KEY_LEN || public_key[0] != 0x04u) {
        return false;
    }

    uint8_t raw[ZV_HERMES_SIG_RAW_LEN];
    size_t raw_len = 0u;
    if (!decode_b64url(signature, signature_len, raw, sizeof(raw), &raw_len) || raw_len != ZV_HERMES_SIG_RAW_LEN) {
        return false;
    }

    if (psa_crypto_init() != PSA_SUCCESS) return false;

    uint8_t digest[ZV_HERMES_DIGEST_LEN];
    size_t digest_len = sizeof(digest);
    if (psa_hash_compute(PSA_ALG_SHA_256, (const uint8_t *)canonical, canonical_len, digest, sizeof(digest),
                          &digest_len) != PSA_SUCCESS) {
        return false;
    }

    /* A volatile public key is used deliberately: nothing is persisted, so a
     * compromised gateway cannot leave usable key material behind. */
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_VERIFY_HASH);
    psa_set_key_algorithm(&attributes, PSA_ALG_ECDSA(PSA_ALG_SHA_256));

    mbedtls_svc_key_id_t key = MBEDTLS_SVC_KEY_ID_INIT;
    if (psa_import_key(&attributes, public_key, public_key_len, &key) != PSA_SUCCESS) {
        /* Also refuses a point that is not on the curve. */
        return false;
    }

    psa_status_t status = psa_verify_hash(key, PSA_ALG_ECDSA(PSA_ALG_SHA_256), digest, digest_len, raw, raw_len);
    psa_destroy_key(key);

    /* Clear the key-derived material before returning either way. */
    memset(digest, 0, sizeof(digest));
    memset(raw, 0, sizeof(raw));
    return status == PSA_SUCCESS;
}

esp_err_t zv_hermes_key_from_b64(const char *kid, const char *public_key_sec1_b64url, zv_trust_key_t *out)
{
    if (kid == NULL || public_key_sec1_b64url == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    if (snprintf(out->kid, sizeof(out->kid), "%s", kid) < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t key_len = 0u;
    if (!decode_b64url(public_key_sec1_b64url, strlen(public_key_sec1_b64url), out->public_key,
                       sizeof(out->public_key), &key_len)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (key_len != ZV_TRUST_P256_KEY_LEN) {
        memset(out, 0, sizeof(*out));
        return ESP_ERR_INVALID_ARG;
    }
    out->public_key_len = key_len;
    out->curve = ZV_TRUST_CURVE_ES256;
    return ESP_OK;
}

zv_status_t zv_hermes_check(const zv_approval_request_t *req, const char *signature, size_t signature_len,
                            int64_t now_s, const zv_approval_policy_t *policy, const zv_trust_store_t *trust,
                            zv_nonce_cache_t *nonce_cache)
{
    zv_approval_gate_t gate = {
        .trust = trust,
        .nonce_cache = nonce_cache,
        .verify = zv_hermes_es256_verify,
        .verify_ctx = NULL,
    };
    return zv_approval_verify(req, signature, signature_len, now_s, policy, &gate);
}

/*
 * Committed TEST-ONLY vector from protocol/v1/test-vectors/approval-signature.json.
 * Generated by scripts/gen_approval_test_vector.py; the private keys were
 * discarded at generation time and no production gateway ever signs with these.
 * Embedded only so the verifier can prove itself on real hardware.
 */
#define KAT_KID "gw-approval-test-2026-09"
#define KAT_PUBKEY_B64 "BK7YmwKvtKfPyiBJ83IsmtjXiuwCIlIoYnsjrKhguWA48hQagFYe8kEN0AcE0S3z3IJ0-X-1e9yEX35ttQ-q6k8"
#define KAT_SIG_B64                                                                                               \
    "sX_qw6aUiDDuoBWat4DmJGjpuZ7zVqYZSk2fWMQ29fqTwBFbNRi2K3o5HTZaimrPUPXRzb15JFn_int5cpP4lQ"
#define KAT_APPROVAL_ID "3f6c2a1e-8b4d-4c7a-9e21-5d0f7b3a9c14"
#define KAT_ACTION "files.delete"
#define KAT_RISK "destructive"
#define KAT_SUMMARY "Delete 14 files in /projects/archive/2025 on DevBox"
#define KAT_ACTION_DIGEST                                                                                          \
    "a0eddf5f86760a48b15b173a7e444c9fe3d79c92f3f061bbbf9c57497d9ad041"
#define KAT_NONCE "q3JtV8x0bF2nYw7pLk4sZg"
#define KAT_ISSUED_AT_S 1789128000LL
#define KAT_EXPIRES_AT_S 1789128045LL
#define KAT_NOW_S (KAT_ISSUED_AT_S + 1LL)

static void kat_request(zv_approval_request_t *req)
{
    memset(req, 0, sizeof(*req));
    snprintf(req->approval_id, sizeof(req->approval_id), "%s", KAT_APPROVAL_ID);
    snprintf(req->action, sizeof(req->action), "%s", KAT_ACTION);
    snprintf(req->risk, sizeof(req->risk), "%s", KAT_RISK);
    snprintf(req->summary, sizeof(req->summary), "%s", KAT_SUMMARY);
    snprintf(req->action_digest, sizeof(req->action_digest), "%s", KAT_ACTION_DIGEST);
    snprintf(req->nonce, sizeof(req->nonce), "%s", KAT_NONCE);
    snprintf(req->kid, sizeof(req->kid), "%s", KAT_KID);
    req->issued_at_s = KAT_ISSUED_AT_S;
    req->expires_at_s = KAT_EXPIRES_AT_S;
}

esp_err_t zv_hermes_selftest(void)
{
    zv_trust_store_t trust;
    zv_trust_key_t key;
    zv_nonce_cache_t nonces;
    zv_approval_policy_t policy = zv_approval_default_policy();
    zv_approval_request_t req;
    int failures = 0;

    zv_trust_store_init(&trust);
    zv_nonce_cache_init(&nonces);

    if (zv_hermes_key_from_b64(KAT_KID, KAT_PUBKEY_B64, &key) != ESP_OK) {
        ESP_LOGE(TAG, "M3 selftest: could not decode the known-answer public key");
        return ESP_FAIL;
    }
    if (zv_trust_store_add(&trust, &key) != ZV_OK) {
        ESP_LOGE(TAG, "M3 selftest: known-answer key refused by the trust store");
        return ESP_FAIL;
    }

    /* 1. A genuine signed request must verify. */
    kat_request(&req);
    zv_status_t status = zv_hermes_check(&req, KAT_SIG_B64, strlen(KAT_SIG_B64), KAT_NOW_S, &policy, &trust, &nonces);
    ESP_LOGI(TAG, "M3 selftest: genuine request -> %d", (int)status);
    if (status != ZV_OK) {
        failures++;
    }

    /* 2. A mutated summary changes the canonical bytes, so the signature fails. */
    kat_request(&req);
    snprintf(req.summary, sizeof(req.summary), "%s", "Delete 1 file in /tmp on DevBox");
    status = zv_hermes_check(&req, KAT_SIG_B64, strlen(KAT_SIG_B64), KAT_NOW_S, &policy, &trust, &nonces);
    ESP_LOGI(TAG, "M3 selftest: mutated action -> %d", (int)status);
    if (status != ZV_ERR_SIGNATURE) {
        failures++;
    }

    /* 3. A tampered signature must fail. */
    kat_request(&req);
    char tampered[ZV_APPROVAL_SIG_MAX + 1u];
    snprintf(tampered, sizeof(tampered), "%s", KAT_SIG_B64);
    tampered[0] = (tampered[0] == 'A') ? 'B' : 'A';
    status = zv_hermes_check(&req, tampered, strlen(tampered), KAT_NOW_S, &policy, &trust, &nonces);
    ESP_LOGI(TAG, "M3 selftest: tampered signature -> %d", (int)status);
    if (status != ZV_ERR_SIGNATURE) {
        failures++;
    }

    /* 4. Replaying the genuine request must be refused. */
    kat_request(&req);
    status = zv_hermes_check(&req, KAT_SIG_B64, strlen(KAT_SIG_B64), KAT_NOW_S, &policy, &trust, &nonces);
    ESP_LOGI(TAG, "M3 selftest: replay -> %d", (int)status);
    if (status != ZV_ERR_REPLAY) {
        failures++;
    }

    /* 5. An unknown kid must be refused without reaching the verifier. */
    zv_nonce_cache_init(&nonces);
    kat_request(&req);
    snprintf(req.kid, sizeof(req.kid), "%s", "gw-key-never-installed");
    status = zv_hermes_check(&req, KAT_SIG_B64, strlen(KAT_SIG_B64), KAT_NOW_S, &policy, &trust, &nonces);
    ESP_LOGI(TAG, "M3 selftest: unknown kid -> %d", (int)status);
    if (status != ZV_ERR_SIGNATURE) {
        failures++;
    }

    /* 6. An expired request must be refused before any crypto. */
    kat_request(&req);
    status = zv_hermes_check(&req, KAT_SIG_B64, strlen(KAT_SIG_B64), KAT_EXPIRES_AT_S, &policy, &trust, &nonces);
    ESP_LOGI(TAG, "M3 selftest: expired -> %d", (int)status);
    if (status != ZV_ERR_EXPIRED) {
        failures++;
    }

    /* 7. Retiring the key must make even the genuine request fail closed. */
    if (zv_trust_store_retire(&trust, KAT_KID) != ZV_OK) {
        failures++;
    }
    zv_nonce_cache_init(&nonces);
    kat_request(&req);
    status = zv_hermes_check(&req, KAT_SIG_B64, strlen(KAT_SIG_B64), KAT_NOW_S, &policy, &trust, &nonces);
    ESP_LOGI(TAG, "M3 selftest: retired key -> %d", (int)status);
    if (status != ZV_ERR_SIGNATURE) {
        failures++;
    }

    if (failures != 0) {
        ESP_LOGE(TAG, "M3 approval selftest FAILED: %d check(s) behaved incorrectly", failures);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "M3 approval selftest passed: signature verify, tamper, replay, unknown kid, expiry, retirement");
    return ESP_OK;
}

/* Adapter for zv_device_command_ctx_t.digest_verify. Core constructs the bytes;
 * this layer computes SHA-256, independently of the sender's claimed digest. */
bool zv_hermes_command_digest_verify(const char *canonical, size_t canonical_len, const char *expected_hex, void *ctx)
{
    (void)ctx;
    if (canonical == NULL || expected_hex == NULL || canonical_len == 0u ||
        strlen(expected_hex) != ZV_APPROVAL_DIGEST_LEN || psa_crypto_init() != PSA_SUCCESS) return false;
    uint8_t digest[32];
    size_t digest_len = 0u;
    if (psa_hash_compute(PSA_ALG_SHA_256, (const uint8_t *)canonical, canonical_len,
                         digest, sizeof(digest), &digest_len) != PSA_SUCCESS || digest_len != sizeof(digest)) return false;
    static const char hex[] = "0123456789abcdef";
    unsigned int difference = 0u;
    for (size_t i = 0u; i < sizeof(digest); i++) {
        difference |= (unsigned int)((unsigned char)expected_hex[2u * i] ^ (unsigned char)hex[digest[i] >> 4]);
        difference |= (unsigned int)((unsigned char)expected_hex[2u * i + 1u] ^ (unsigned char)hex[digest[i] & 15u]);
    }
    memset(digest, 0, sizeof(digest));
    return difference == 0u;
}
