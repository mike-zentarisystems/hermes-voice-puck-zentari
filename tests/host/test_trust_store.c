/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 *
 * Trust store and the ordered approval verification gate.
 *
 * The properties worth proving here are negative ones: that an unauthenticated
 * peer cannot reach the signature checker, cannot consume a nonce slot, and
 * cannot turn an approval into a decision. A gate that only has a happy-path
 * test reads as evidence without being any.
 */
#include "approval_vector.h"
#include "zv_approval.h"
#include "zv_app_state.h"
#include "zv_event.h"
#include "zv_test.h"

#define FAKE_SIG_LEN 86u /**< base64url length of a raw 64-byte ES256 signature */

/* ---------------------------------------------------------------- helpers */

static void fill_ascii(char *buf, size_t len, char fill)
{
    for (size_t i = 0u; i < len; i++) {
        buf[i] = fill;
    }
    buf[len] = '\0';
}

static zv_trust_key_t make_key(const char *kid, int64_t not_before_s, int64_t not_after_s)
{
    zv_trust_key_t key;
    memset(&key, 0, sizeof(key));
    snprintf(key.kid, sizeof(key.kid), "%s", kid);
    key.curve = ZV_TRUST_CURVE_ES256;
    key.public_key[0] = 0x04u;
    for (size_t i = 1u; i < ZV_TRUST_P256_KEY_LEN; i++) {
        key.public_key[i] = (uint8_t)i;
    }
    key.public_key_len = ZV_TRUST_P256_KEY_LEN;
    key.not_before_s = not_before_s;
    key.not_after_s = not_after_s;
    return key;
}

static zv_approval_request_t vector_request(void)
{
    zv_approval_request_t req;
    memset(&req, 0, sizeof(req));
    snprintf(req.approval_id, sizeof(req.approval_id), "%s", ZV_TV_APPROVAL_ID);
    snprintf(req.action, sizeof(req.action), "%s", ZV_TV_ACTION);
    snprintf(req.risk, sizeof(req.risk), "%s", ZV_TV_RISK);
    snprintf(req.summary, sizeof(req.summary), "%s", ZV_TV_SUMMARY);
    snprintf(req.action_digest, sizeof(req.action_digest), "%s", ZV_TV_ACTION_DIGEST);
    snprintf(req.nonce, sizeof(req.nonce), "%s", ZV_TV_NONCE);
    snprintf(req.kid, sizeof(req.kid), "%s", ZV_TV_KID);
    req.issued_at_s = ZV_TV_ISSUED_AT_S;
    req.expires_at_s = ZV_TV_EXPIRES_AT_S;
    return req;
}

/** Records what the gate handed to the platform verifier. */
typedef struct {
    bool accept;
    int calls;
    size_t last_len;
    char last_canonical[ZV_APPROVAL_CANONICAL_MAX];
    uint8_t last_key_first;
} verifier_log_t;

static bool fake_verify(const uint8_t *public_key, size_t public_key_len, const char *canonical,
                        size_t canonical_len, const char *signature, size_t signature_len, void *ctx)
{
    verifier_log_t *log = (verifier_log_t *)ctx;
    (void)signature;
    (void)signature_len;
    log->calls++;
    log->last_len = canonical_len;
    log->last_key_first = (public_key_len > 0u) ? public_key[0] : 0u;
    if (canonical_len <= sizeof(log->last_canonical)) {
        memcpy(log->last_canonical, canonical, canonical_len);
        log->last_canonical[canonical_len] = '\0';
    }
    return log->accept;
}

typedef struct {
    zv_trust_store_t trust;
    zv_nonce_cache_t nonce;
    verifier_log_t log;
    zv_approval_gate_t gate;
} fixture_t;

static void fixture_init(fixture_t *f, bool accept)
{
    memset(f, 0, sizeof(*f));
    zv_trust_store_init(&f->trust);
    zv_nonce_cache_init(&f->nonce);
    f->log.accept = accept;
    zv_trust_key_t key = make_key(ZV_TV_KID, 0, 0);
    ZV_CHECK_EQ(zv_trust_store_add(&f->trust, &key), ZV_OK);
    f->gate.trust = &f->trust;
    f->gate.nonce_cache = &f->nonce;
    f->gate.verify = fake_verify;
    f->gate.verify_ctx = &f->log;
}

/* ------------------------------------------------------------ trust store */

static void test_trust_store_add_validation(void)
{
    zv_trust_store_t store;
    zv_trust_store_init(&store);
    ZV_CHECK_EQ(zv_trust_store_count(&store), 0);
    ZV_CHECK(zv_trust_store_find(&store, ZV_TV_KID, 1000) == NULL);
    ZV_CHECK_EQ(zv_trust_store_count(NULL), 0);

    zv_trust_key_t key = make_key("gw-key-1", 0, 0);
    ZV_CHECK_EQ(zv_trust_store_add(NULL, &key), ZV_ERR_INVALID_ARG);
    ZV_CHECK_EQ(zv_trust_store_add(&store, NULL), ZV_ERR_INVALID_ARG);

    ZV_CHECK_EQ(zv_trust_store_add(&store, &key), ZV_OK);
    ZV_CHECK_EQ(zv_trust_store_count(&store), 1);
    /* A duplicate kid must not silently replace the key that signed in flight. */
    ZV_CHECK_EQ(zv_trust_store_add(&store, &key), ZV_ERR_INVALID_STATE);

    zv_trust_key_t bad = make_key("gw-key-2", 0, 0);
    bad.curve = (zv_trust_curve_t)7;
    ZV_CHECK_EQ(zv_trust_store_add(&store, &bad), ZV_ERR_INVALID_ARG);

    bad = make_key("gw-key-2", 0, 0);
    bad.public_key_len = 64u;
    ZV_CHECK_EQ(zv_trust_store_add(&store, &bad), ZV_ERR_INVALID_ARG);

    bad = make_key("gw-key-2", 0, 0);
    bad.public_key[0] = 0x02u; /* compressed point, not accepted */
    ZV_CHECK_EQ(zv_trust_store_add(&store, &bad), ZV_ERR_INVALID_ARG);

    bad = make_key("", 0, 0);
    ZV_CHECK_EQ(zv_trust_store_add(&store, &bad), ZV_ERR_INVALID_ARG);

    bad = make_key("bad key", 0, 0); /* space is outside the kid charset */
    ZV_CHECK_EQ(zv_trust_store_add(&store, &bad), ZV_ERR_INVALID_ARG);

    bad = make_key("gw-key-2", 500, 500); /* empty window */
    ZV_CHECK_EQ(zv_trust_store_add(&store, &bad), ZV_ERR_INVALID_ARG);

    bad = make_key("gw-key-2", 900, 800); /* inverted window */
    ZV_CHECK_EQ(zv_trust_store_add(&store, &bad), ZV_ERR_INVALID_ARG);

    ZV_CHECK_EQ(zv_trust_store_count(&store), 1);
}

static void test_trust_store_capacity_and_retire(void)
{
    zv_trust_store_t store;
    zv_trust_store_init(&store);
    for (size_t i = 0u; i < ZV_TRUST_MAX_KEYS; i++) {
        char kid[32];
        snprintf(kid, sizeof(kid), "gw-key-%02zu", i);
        zv_trust_key_t key = make_key(kid, 0, 0);
        ZV_CHECK_EQ(zv_trust_store_add(&store, &key), ZV_OK);
    }
    zv_trust_key_t overflow = make_key("gw-key-overflow", 0, 0);
    ZV_CHECK_EQ(zv_trust_store_add(&store, &overflow), ZV_ERR_QUEUE_FULL);

    ZV_CHECK(zv_trust_store_find(&store, "gw-key-00", 1000) != NULL);
    ZV_CHECK_EQ(zv_trust_store_retire(&store, "gw-key-00"), ZV_OK);
    ZV_CHECK(zv_trust_store_find(&store, "gw-key-00", 1000) == NULL);
    ZV_CHECK_EQ(zv_trust_store_count(&store), ZV_TRUST_MAX_KEYS - 1u);
    ZV_CHECK_EQ(zv_trust_store_retire(&store, "gw-key-00"), ZV_ERR_INVALID_STATE);
    ZV_CHECK_EQ(zv_trust_store_retire(NULL, "gw-key-01"), ZV_ERR_INVALID_ARG);

    /* Retiring must free a slot rather than leave the store permanently full. */
    ZV_CHECK_EQ(zv_trust_store_add(&store, &overflow), ZV_OK);
}

static void test_trust_store_rotation_window(void)
{
    zv_trust_store_t store;
    zv_trust_store_init(&store);
    zv_trust_key_t old_key = make_key("gw-old", 0, 1000);
    zv_trust_key_t new_key = make_key("gw-new", 900, 0);
    ZV_CHECK_EQ(zv_trust_store_add(&store, &old_key), ZV_OK);
    ZV_CHECK_EQ(zv_trust_store_add(&store, &new_key), ZV_OK);

    /* Overlap: both keys verify while approvals signed by the old key drain. */
    ZV_CHECK(zv_trust_store_find(&store, "gw-old", 950) != NULL);
    ZV_CHECK(zv_trust_store_find(&store, "gw-new", 950) != NULL);
    /* Bounds are inclusive-start, exclusive-end. */
    ZV_CHECK(zv_trust_store_find(&store, "gw-old", 999) != NULL);
    ZV_CHECK(zv_trust_store_find(&store, "gw-old", 1000) == NULL);
    ZV_CHECK(zv_trust_store_find(&store, "gw-new", 899) == NULL);
    ZV_CHECK(zv_trust_store_find(&store, "gw-new", 900) != NULL);
    ZV_CHECK(zv_trust_store_find(&store, "gw-absent", 950) == NULL);

    /* A NULL kid is refused rather than dereferenced. */
    ZV_CHECK(zv_trust_store_find(&store, NULL, 950) == NULL);
    ZV_CHECK(zv_trust_store_find(NULL, "gw-old", 950) == NULL);
    ZV_CHECK_EQ(zv_trust_store_retire(&store, NULL), ZV_ERR_INVALID_ARG);

    /* A kid matching but out of window must not fall through to another key. */
    ZV_CHECK(zv_trust_store_find(&store, "gw-old", 1000) == NULL);
    ZV_CHECK_STR(zv_trust_curve_name(ZV_TRUST_CURVE_ES256), "ES256");
    ZV_CHECK_STR(zv_trust_curve_name((zv_trust_curve_t)9), "unknown");
}

/* ------------------------------------------------------- the verify gate */

static void test_verify_happy_path(void)
{
    fixture_t f;
    fixture_init(&f, true);
    char sig[FAKE_SIG_LEN + 1u];
    fill_ascii(sig, FAKE_SIG_LEN, 'A');
    zv_approval_request_t req = vector_request();
    zv_approval_policy_t policy = zv_approval_default_policy();

    ZV_CHECK_EQ(zv_approval_verify(&req, sig, FAKE_SIG_LEN, ZV_TV_ISSUED_AT_S + 1, &policy, &f.gate), ZV_OK);
    ZV_CHECK_EQ(f.log.calls, 1);
    /* The platform must receive the exact bytes the gateway signed. */
    ZV_CHECK_STR(f.log.last_canonical, ZV_TV_CANONICAL);
    ZV_CHECK_EQ(f.log.last_len, strlen(ZV_TV_CANONICAL));
    ZV_CHECK_EQ(f.log.last_key_first, 0x04);

    /* Same request again is a replay, now that the nonce is spent. */
    ZV_CHECK_EQ(zv_approval_verify(&req, sig, FAKE_SIG_LEN, ZV_TV_ISSUED_AT_S + 2, &policy, &f.gate),
                ZV_ERR_REPLAY);
}

static void test_verify_rejects_before_crypto(void)
{
    char sig[FAKE_SIG_LEN + 1u];
    fill_ascii(sig, FAKE_SIG_LEN, 'A');
    zv_approval_policy_t policy = zv_approval_default_policy();

    /* Expired: never reaches the verifier. */
    fixture_t expired_fixture;
    fixture_init(&expired_fixture, true);
    zv_approval_request_t req = vector_request();
    ZV_CHECK_EQ(zv_approval_verify(&req, sig, FAKE_SIG_LEN, ZV_TV_EXPIRES_AT_S, &policy, &expired_fixture.gate),
                ZV_ERR_EXPIRED);
    ZV_CHECK_EQ(expired_fixture.log.calls, 0);

    /* Issued too far in the future. */
    fixture_t future_fixture;
    fixture_init(&future_fixture, true);
    req = vector_request();
    ZV_CHECK_EQ(zv_approval_verify(&req, sig, FAKE_SIG_LEN, ZV_TV_ISSUED_AT_S - 60, &policy, &future_fixture.gate),
                ZV_ERR_NOT_YET_VALID);
    ZV_CHECK_EQ(future_fixture.log.calls, 0);

    /* TTL longer than policy allows. */
    fixture_t ttl_fixture;
    fixture_init(&ttl_fixture, true);
    req = vector_request();
    req.expires_at_s = req.issued_at_s + 61;
    ZV_CHECK_EQ(zv_approval_verify(&req, sig, FAKE_SIG_LEN, ZV_TV_ISSUED_AT_S + 1, &policy, &ttl_fixture.gate),
                ZV_ERR_POLICY);
    ZV_CHECK_EQ(ttl_fixture.log.calls, 0);

    /* Non-destructive risk is not an approval at all. */
    fixture_t risk_fixture;
    fixture_init(&risk_fixture, true);
    req = vector_request();
    snprintf(req.risk, sizeof(req.risk), "read-only");
    ZV_CHECK_EQ(zv_approval_verify(&req, sig, FAKE_SIG_LEN, ZV_TV_ISSUED_AT_S + 1, &policy, &risk_fixture.gate),
                ZV_ERR_INVALID_ARG);
    ZV_CHECK_EQ(risk_fixture.log.calls, 0);
}

static void test_verify_signature_and_key_negatives(void)
{
    char sig[FAKE_SIG_LEN + 1u];
    zv_approval_policy_t policy = zv_approval_default_policy();

    /* Malformed signature text and length never reach the verifier. */
    fixture_t charset_fixture;
    fixture_init(&charset_fixture, true);
    fill_ascii(sig, FAKE_SIG_LEN, '+'); /* '+' is base64, not base64url */
    zv_approval_request_t req = vector_request();
    ZV_CHECK_EQ(zv_approval_verify(&req, sig, FAKE_SIG_LEN, ZV_TV_ISSUED_AT_S + 1, &policy, &charset_fixture.gate),
                ZV_ERR_INVALID_ARG);
    ZV_CHECK_EQ(charset_fixture.log.calls, 0);

    fixture_t short_fixture;
    fixture_init(&short_fixture, true);
    fill_ascii(sig, FAKE_SIG_LEN, 'A');
    ZV_CHECK_EQ(zv_approval_verify(&req, sig, 10u, ZV_TV_ISSUED_AT_S + 1, &policy, &short_fixture.gate),
                ZV_ERR_INVALID_ARG);
    ZV_CHECK_EQ(short_fixture.log.calls, 0);

    /* A verifier that rejects the signature must not spend the nonce. */
    fixture_t bad_sig;
    fixture_init(&bad_sig, false);
    ZV_CHECK_EQ(zv_approval_verify(&req, sig, FAKE_SIG_LEN, ZV_TV_ISSUED_AT_S + 1, &policy, &bad_sig.gate),
                ZV_ERR_SIGNATURE);
    ZV_CHECK_EQ(bad_sig.log.calls, 1);
    /* A rejected signature left the cache free, so the same nonce still verifies. */
    fixture_init(&bad_sig, true);
    ZV_CHECK_EQ(zv_approval_verify(&req, sig, FAKE_SIG_LEN, ZV_TV_ISSUED_AT_S + 1, &policy, &bad_sig.gate), ZV_OK);

    /* Unknown kid and retired kid both read as a bad signature. */
    fixture_t unknown_kid;
    fixture_init(&unknown_kid, true);
    req = vector_request();
    snprintf(req.kid, sizeof(req.kid), "gw-key-never-installed");
    ZV_CHECK_EQ(zv_approval_verify(&req, sig, FAKE_SIG_LEN, ZV_TV_ISSUED_AT_S + 1, &policy, &unknown_kid.gate),
                ZV_ERR_SIGNATURE);
    ZV_CHECK_EQ(unknown_kid.log.calls, 0);

    fixture_t retired;
    fixture_init(&retired, true);
    ZV_CHECK_EQ(zv_trust_store_retire(&retired.trust, ZV_TV_KID), ZV_OK);
    req = vector_request();
    ZV_CHECK_EQ(zv_approval_verify(&req, sig, FAKE_SIG_LEN, ZV_TV_ISSUED_AT_S + 1, &policy, &retired.gate),
                ZV_ERR_SIGNATURE);
    ZV_CHECK_EQ(retired.log.calls, 0);

    /* A kid that exists but is not yet valid must not fall through. */
    fixture_t not_yet;
    memset(&not_yet, 0, sizeof(not_yet));
    zv_trust_store_init(&not_yet.trust);
    zv_nonce_cache_init(&not_yet.nonce);
    not_yet.log.accept = true;
    zv_trust_key_t future_key = make_key(ZV_TV_KID, ZV_TV_ISSUED_AT_S + 500, 0);
    ZV_CHECK_EQ(zv_trust_store_add(&not_yet.trust, &future_key), ZV_OK);
    not_yet.gate.trust = &not_yet.trust;
    not_yet.gate.nonce_cache = &not_yet.nonce;
    not_yet.gate.verify = fake_verify;
    not_yet.gate.verify_ctx = &not_yet.log;
    req = vector_request();
    ZV_CHECK_EQ(zv_approval_verify(&req, sig, FAKE_SIG_LEN, ZV_TV_ISSUED_AT_S + 1, &policy, &not_yet.gate),
                ZV_ERR_SIGNATURE);
    ZV_CHECK_EQ(not_yet.log.calls, 0);
}

static void test_verify_forgery_cannot_poison_nonce_cache(void)
{
    char sig[FAKE_SIG_LEN + 1u];
    fill_ascii(sig, FAKE_SIG_LEN, 'A');
    zv_approval_policy_t policy = zv_approval_default_policy();
    zv_approval_request_t genuine = vector_request();

    fixture_t f;
    fixture_init(&f, false); /* the verifier rejects everything */

    /* Flood the cache with forged requests that reuse the genuine nonce. */
    for (int i = 0; i < 4; i++) {
        zv_approval_request_t forged = vector_request();
        snprintf(forged.approval_id, sizeof(forged.approval_id), "00000000-0000-4000-8000-00000000000%d", i);
        ZV_CHECK_EQ(zv_approval_verify(&forged, sig, FAKE_SIG_LEN, ZV_TV_ISSUED_AT_S + 1, &policy, &f.gate),
                    ZV_ERR_SIGNATURE);
    }
    ZV_CHECK_EQ(f.log.calls, 4);

    /* The genuine request still verifies once a real signature arrives, which
     * proves the forgeries consumed no nonce slots. */
    f.log.accept = true;
    ZV_CHECK_EQ(zv_approval_verify(&genuine, sig, FAKE_SIG_LEN, ZV_TV_ISSUED_AT_S + 1, &policy, &f.gate), ZV_OK);
}

static void test_verify_argument_checks(void)
{
    fixture_t f;
    fixture_init(&f, true);
    char sig[FAKE_SIG_LEN + 1u];
    fill_ascii(sig, FAKE_SIG_LEN, 'A');
    zv_approval_request_t req = vector_request();
    zv_approval_policy_t policy = zv_approval_default_policy();

    ZV_CHECK_EQ(zv_approval_verify(NULL, sig, FAKE_SIG_LEN, 1, &policy, &f.gate), ZV_ERR_INVALID_ARG);
    ZV_CHECK_EQ(zv_approval_verify(&req, NULL, FAKE_SIG_LEN, 1, &policy, &f.gate), ZV_ERR_INVALID_ARG);
    ZV_CHECK_EQ(zv_approval_verify(&req, sig, FAKE_SIG_LEN, 1, NULL, &f.gate), ZV_ERR_INVALID_ARG);
    ZV_CHECK_EQ(zv_approval_verify(&req, sig, FAKE_SIG_LEN, 1, &policy, NULL), ZV_ERR_INVALID_ARG);

    zv_approval_gate_t missing = f.gate;
    missing.verify = NULL;
    ZV_CHECK_EQ(zv_approval_verify(&req, sig, FAKE_SIG_LEN, ZV_TV_ISSUED_AT_S + 1, &policy, &missing),
                ZV_ERR_INVALID_ARG);
    missing = f.gate;
    missing.trust = NULL;
    ZV_CHECK_EQ(zv_approval_verify(&req, sig, FAKE_SIG_LEN, ZV_TV_ISSUED_AT_S + 1, &policy, &missing),
                ZV_ERR_INVALID_ARG);
    missing = f.gate;
    missing.nonce_cache = NULL;
    ZV_CHECK_EQ(zv_approval_verify(&req, sig, FAKE_SIG_LEN, ZV_TV_ISSUED_AT_S + 1, &policy, &missing),
                ZV_ERR_INVALID_ARG);
}

/* ------------------------------------------------- response and decisions */

static void test_response_build(void)
{
    char buf[ZV_APPROVAL_RESPONSE_MAX];

    ZV_CHECK_EQ(zv_approval_response_build(buf, sizeof(buf), ZV_TV_APPROVAL_ID, ZV_APPROVAL_OUTCOME_APPROVED,
                                           "puck-lab-0001", ZV_TV_ISSUED_AT_S + 7),
                ZV_OK);
    ZV_CHECK_STR(buf,
                 "{\"v\":1,\"type\":\"approval.response\",\"approval_id\":\"" ZV_TV_APPROVAL_ID
                 "\",\"decision\":\"approved\",\"device_id\":\"puck-lab-0001\",\"decided_at\":1789128007}");

    ZV_CHECK_EQ(zv_approval_response_build(buf, sizeof(buf), ZV_TV_APPROVAL_ID, ZV_APPROVAL_OUTCOME_DENIED,
                                           "puck-lab-0001", 1),
                ZV_OK);
    ZV_CHECK(strstr(buf, "\"decision\":\"denied\"") != NULL);

    ZV_CHECK_EQ(zv_approval_response_build(buf, sizeof(buf), ZV_TV_APPROVAL_ID, ZV_APPROVAL_OUTCOME_EXPIRED,
                                           "puck-lab-0001", 1),
                ZV_OK);
    ZV_CHECK(strstr(buf, "\"decision\":\"expired\"") != NULL);

    char tiny[32];
    ZV_CHECK_EQ(zv_approval_response_build(tiny, sizeof(tiny), ZV_TV_APPROVAL_ID, ZV_APPROVAL_OUTCOME_APPROVED,
                                           "puck-lab-0001", 1),
                ZV_ERR_BUFFER_TOO_SMALL);

    /* An out-of-charset or wrongly-cased device id must not be sent. */
    ZV_CHECK_EQ(zv_approval_response_build(buf, sizeof(buf), ZV_TV_APPROVAL_ID, ZV_APPROVAL_OUTCOME_APPROVED,
                                           "Puck-Lab-0001", 1),
                ZV_ERR_INVALID_ARG);
    ZV_CHECK_EQ(zv_approval_response_build(buf, sizeof(buf), ZV_TV_APPROVAL_ID, ZV_APPROVAL_OUTCOME_APPROVED,
                                           "short", 1),
                ZV_ERR_INVALID_ARG);
    ZV_CHECK_EQ(zv_approval_response_build(buf, sizeof(buf), "not-a-uuid", ZV_APPROVAL_OUTCOME_APPROVED,
                                           "puck-lab-0001", 1),
                ZV_ERR_INVALID_ARG);
    ZV_CHECK_EQ(zv_approval_response_build(buf, sizeof(buf), ZV_TV_APPROVAL_ID, ZV_APPROVAL_OUTCOME_APPROVED,
                                           "puck-lab-0001", 0),
                ZV_ERR_INVALID_ARG);
    ZV_CHECK_EQ(zv_approval_response_build(buf, sizeof(buf), ZV_TV_APPROVAL_ID, (zv_approval_outcome_t)9,
                                           "puck-lab-0001", 1),
                ZV_ERR_INVALID_ARG);
    ZV_CHECK_EQ(zv_approval_response_build(NULL, sizeof(buf), ZV_TV_APPROVAL_ID, ZV_APPROVAL_OUTCOME_APPROVED,
                                           "puck-lab-0001", 1),
                ZV_ERR_INVALID_ARG);
}

static void test_hold_to_decision(void)
{
    zv_approval_outcome_t outcome = ZV_APPROVAL_OUTCOME_DENIED;
    ZV_CHECK(zv_approval_decision_from_hold(ZV_HOLD_CONFIRMED, &outcome));
    ZV_CHECK_EQ(outcome, ZV_APPROVAL_OUTCOME_APPROVED);
    ZV_CHECK(zv_approval_decision_from_hold(ZV_HOLD_DENIED, &outcome));
    ZV_CHECK_EQ(outcome, ZV_APPROVAL_OUTCOME_DENIED);
    ZV_CHECK(zv_approval_decision_from_hold(ZV_HOLD_EXPIRED, &outcome));
    ZV_CHECK_EQ(outcome, ZV_APPROVAL_OUTCOME_EXPIRED);

    /* A partial hold must never produce a decision. */
    ZV_CHECK(!zv_approval_decision_from_hold(ZV_HOLD_ARMING, &outcome));
    ZV_CHECK(!zv_approval_decision_from_hold(ZV_HOLD_READY, &outcome));
    ZV_CHECK(!zv_approval_decision_from_hold(ZV_HOLD_PRESSING, &outcome));
    ZV_CHECK(!zv_approval_decision_from_hold(ZV_HOLD_CONFIRMED, NULL));

    ZV_CHECK_STR(zv_approval_decision_name(ZV_APPROVAL_OUTCOME_APPROVED), "approved");
    ZV_CHECK_STR(zv_approval_decision_name((zv_approval_outcome_t)9), "unknown");
}

/** An approved hold must reach ZV_STATE_EXECUTING, a denied one must not. */
static void test_approval_state_outcome(void)
{
    zv_event_t event;
    zv_app_state_t next = ZV_STATE_BOOT;
    memset(&event, 0, sizeof(event));
    event.type = ZV_EVT_APPROVAL_RESOLVED;

    event.data.approval.outcome = ZV_APPROVAL_OUTCOME_APPROVED;
    ZV_CHECK_EQ(zv_app_state_next(ZV_STATE_APPROVAL, &event, &next), ZV_OK);
    ZV_CHECK_EQ(next, ZV_STATE_EXECUTING);

    event.data.approval.outcome = ZV_APPROVAL_OUTCOME_DENIED;
    ZV_CHECK_EQ(zv_app_state_next(ZV_STATE_APPROVAL, &event, &next), ZV_OK);
    ZV_CHECK_EQ(next, ZV_STATE_IDLE);

    event.data.approval.outcome = ZV_APPROVAL_OUTCOME_EXPIRED;
    ZV_CHECK_EQ(zv_app_state_next(ZV_STATE_APPROVAL, &event, &next), ZV_OK);
    ZV_CHECK_EQ(next, ZV_STATE_IDLE);
}

void test_suite_trust_store(void)
{
    test_trust_store_add_validation();
    test_trust_store_capacity_and_retire();
    test_trust_store_rotation_window();
    test_verify_happy_path();
    test_verify_rejects_before_crypto();
    test_verify_signature_and_key_negatives();
    test_verify_forgery_cannot_poison_nonce_cache();
    test_verify_argument_checks();
    test_response_build();
    test_hold_to_decision();
    test_approval_state_outcome();
}