/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 */
#include "approval_vector.h"
#include "zv_approval.h"
#include "zv_test.h"

static void copy_field(char *dst, size_t dst_size, const char *src)
{
    size_t n = strlen(src);
    if (n >= dst_size) {
        n = dst_size - 1u;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static zv_approval_request_t vector_request(void)
{
    zv_approval_request_t req;
    memset(&req, 0, sizeof(req));
    copy_field(req.approval_id, sizeof(req.approval_id), ZV_TV_APPROVAL_ID);
    copy_field(req.action, sizeof(req.action), ZV_TV_ACTION);
    copy_field(req.risk, sizeof(req.risk), ZV_TV_RISK);
    copy_field(req.summary, sizeof(req.summary), ZV_TV_SUMMARY);
    copy_field(req.action_digest, sizeof(req.action_digest), ZV_TV_ACTION_DIGEST);
    copy_field(req.nonce, sizeof(req.nonce), ZV_TV_NONCE);
    copy_field(req.kid, sizeof(req.kid), ZV_TV_KID);
    req.issued_at_s = ZV_TV_ISSUED_AT_S;
    req.expires_at_s = ZV_TV_EXPIRES_AT_S;
    return req;
}

static void test_canonical_matches_protocol_vector(void)
{
    zv_approval_request_t req = vector_request();
    char buf[ZV_APPROVAL_CANONICAL_MAX];
    size_t len = 0;
    ZV_CHECK_EQ(zv_approval_canonical(&req, buf, sizeof(buf), &len), ZV_OK);
    ZV_CHECK_STR(buf, ZV_TV_CANONICAL);
    ZV_CHECK_EQ(len, strlen(ZV_TV_CANONICAL));

    char tiny[16];
    ZV_CHECK_EQ(zv_approval_canonical(&req, tiny, sizeof(tiny), &len), ZV_ERR_BUFFER_TOO_SMALL);
}

static void test_validation(void)
{
    zv_approval_policy_t policy = zv_approval_default_policy();
    zv_approval_request_t req = vector_request();
    ZV_CHECK_EQ(zv_approval_validate(&req, ZV_TV_ISSUED_AT_S + 1, &policy), ZV_OK);
    ZV_CHECK_EQ(zv_approval_validate(&req, ZV_TV_EXPIRES_AT_S, &policy), ZV_ERR_EXPIRED);
    ZV_CHECK_EQ(zv_approval_validate(&req, ZV_TV_ISSUED_AT_S - 4, &policy), ZV_OK);
    ZV_CHECK_EQ(zv_approval_validate(&req, ZV_TV_ISSUED_AT_S - 6, &policy), ZV_ERR_NOT_YET_VALID);

    zv_approval_request_t bad = vector_request();
    bad.expires_at_s = bad.issued_at_s + 61;
    ZV_CHECK_EQ(zv_approval_validate(&bad, ZV_TV_ISSUED_AT_S + 1, &policy), ZV_ERR_POLICY);

    bad = vector_request();
    bad.approval_id[0] = 'A'; /* upper-case hex is not canonical */
    ZV_CHECK_EQ(zv_approval_validate(&bad, ZV_TV_ISSUED_AT_S + 1, &policy), ZV_ERR_INVALID_ARG);

    bad = vector_request();
    bad.summary[3] = '\n'; /* would break canonical framing */
    ZV_CHECK_EQ(zv_approval_validate(&bad, ZV_TV_ISSUED_AT_S + 1, &policy), ZV_ERR_INVALID_ARG);

    bad = vector_request();
    copy_field(bad.risk, sizeof(bad.risk), "harmless");
    ZV_CHECK_EQ(zv_approval_validate(&bad, ZV_TV_ISSUED_AT_S + 1, &policy), ZV_ERR_INVALID_ARG);

    bad = vector_request();
    bad.action_digest[10] = 'G';
    ZV_CHECK_EQ(zv_approval_validate(&bad, ZV_TV_ISSUED_AT_S + 1, &policy), ZV_ERR_INVALID_ARG);

    bad = vector_request();
    copy_field(bad.nonce, sizeof(bad.nonce), "tooShort");
    ZV_CHECK_EQ(zv_approval_validate(&bad, ZV_TV_ISSUED_AT_S + 1, &policy), ZV_ERR_INVALID_ARG);

    bad = vector_request();
    copy_field(bad.action, sizeof(bad.action), "Files.Delete");
    ZV_CHECK_EQ(zv_approval_validate(&bad, ZV_TV_ISSUED_AT_S + 1, &policy), ZV_ERR_INVALID_ARG);

    ZV_CHECK_EQ(zv_approval_validate(NULL, 0, &policy), ZV_ERR_INVALID_ARG);
}

static void test_nonce_cache(void)
{
    zv_nonce_cache_t cache;
    zv_nonce_cache_init(&cache);
    ZV_CHECK_EQ(zv_nonce_cache_check_and_insert(&cache, ZV_TV_NONCE, 1045, 1000), ZV_OK);
    ZV_CHECK_EQ(zv_nonce_cache_check_and_insert(&cache, ZV_TV_NONCE, 1045, 1010), ZV_ERR_REPLAY);
    ZV_CHECK_EQ(zv_nonce_cache_check_and_insert(&cache, ZV_TV_NONCE, 1100, 1045), ZV_OK); /* expired entry */

    zv_nonce_cache_init(&cache);
    char nonce[32];
    for (int i = 0; i < (int)ZV_NONCE_CACHE_SIZE; i++) {
        snprintf(nonce, sizeof(nonce), "nonce-live-entry-%05d", i);
        ZV_CHECK_EQ(zv_nonce_cache_check_and_insert(&cache, nonce, 2000, 1000), ZV_OK);
    }
    ZV_CHECK_EQ(zv_nonce_cache_check_and_insert(&cache, "nonce-live-entry-99999", 2000, 1000), ZV_ERR_QUEUE_FULL);
    ZV_CHECK_EQ(zv_nonce_cache_check_and_insert(&cache, "short", 2000, 1000), ZV_ERR_INVALID_ARG);
}

static void test_hold_to_confirm(void)
{
    zv_hold_confirm_t hold;
    ZV_CHECK_EQ(zv_hold_init(&hold, 0, 400, 1500, 1800), ZV_ERR_INVALID_ARG);
    ZV_CHECK_EQ(zv_hold_init(&hold, 0, 400, 1500, 45000), ZV_OK);

    ZV_CHECK_EQ(zv_hold_press(&hold, 100), ZV_HOLD_ARMING); /* carry-over tap ignored */
    ZV_CHECK_EQ(zv_hold_press(&hold, 500), ZV_HOLD_PRESSING);
    ZV_CHECK_EQ(zv_hold_tick(&hold, 1250), ZV_HOLD_PRESSING);
    ZV_CHECK_EQ(hold.progress_permille, 500);
    ZV_CHECK_EQ(zv_hold_release(&hold, 1300), ZV_HOLD_READY);
    ZV_CHECK_EQ(hold.progress_permille, 0);

    ZV_CHECK_EQ(zv_hold_press(&hold, 2000), ZV_HOLD_PRESSING);
    ZV_CHECK_EQ(zv_hold_tick(&hold, 3500), ZV_HOLD_CONFIRMED);
    ZV_CHECK_EQ(zv_hold_deny(&hold, 3600), ZV_HOLD_CONFIRMED); /* terminal */

    ZV_CHECK_EQ(zv_hold_init(&hold, 0, 400, 1500, 45000), ZV_OK);
    ZV_CHECK_EQ(zv_hold_deny(&hold, 10), ZV_HOLD_DENIED); /* deny allowed while arming */

    ZV_CHECK_EQ(zv_hold_init(&hold, 0, 400, 1500, 45000), ZV_OK);
    ZV_CHECK_EQ(zv_hold_press(&hold, 44000), ZV_HOLD_PRESSING);
    ZV_CHECK_EQ(zv_hold_tick(&hold, 45000), ZV_HOLD_EXPIRED); /* held only 1000 ms at deadline */
    ZV_CHECK(zv_hold_is_terminal(ZV_HOLD_EXPIRED));
}

void test_suite_approval(void)
{
    test_canonical_matches_protocol_vector();
    test_validation();
    test_nonce_cache();
    test_hold_to_confirm();
}
