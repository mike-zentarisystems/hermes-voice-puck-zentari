/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 */
#include "zv_link_supervisor.h"
#include "zv_test.h"

static uint32_t rand_zero(void *ctx)
{
    (void)ctx;
    return 0u;
}

static uint32_t rand_max(void *ctx)
{
    (void)ctx;
    return UINT32_MAX;
}

static const zv_link_config_t k_cfg = {
    .backoff_base_ms = 1000u,
    .backoff_max_ms = 8000u,
    .breaker = {.failure_threshold = 5u, .open_duration_ms = 30000u, .half_open_max_trials = 1u},
    .degraded_after_failures = 3u,
};

static bool poll(zv_link_supervisor_t *s, uint32_t now)
{
    bool attempt = false;
    ZV_CHECK_EQ(zv_link_poll(s, now, &attempt), ZV_OK);
    return attempt;
}

static void test_reconnect_progression(void)
{
    zv_link_supervisor_t s;
    ZV_CHECK_EQ(zv_link_init(&s, &k_cfg, rand_zero, NULL, 0), ZV_OK);
    ZV_CHECK_EQ(zv_link_state(&s), ZV_LINK_OFFLINE);

    ZV_CHECK(poll(&s, 0));
    ZV_CHECK_EQ(zv_link_state(&s), ZV_LINK_CONNECTING);
    ZV_CHECK(!poll(&s, 1)); /* attempt in flight */

    ZV_CHECK_EQ(zv_link_on_failure(&s, 10), ZV_OK); /* wait 500 */
    ZV_CHECK_EQ(zv_link_state(&s), ZV_LINK_OFFLINE);
    ZV_CHECK(!poll(&s, 400));
    ZV_CHECK(poll(&s, 510));

    ZV_CHECK_EQ(zv_link_on_failure(&s, 600), ZV_OK); /* wait 1000 */
    ZV_CHECK(!poll(&s, 1500));
    ZV_CHECK(poll(&s, 1600));
    ZV_CHECK_EQ(zv_link_on_failure(&s, 1700), ZV_OK); /* third failure -> degraded */
    ZV_CHECK_EQ(zv_link_state(&s), ZV_LINK_DEGRADED);

    ZV_CHECK(poll(&s, 3700));
    ZV_CHECK_EQ(zv_link_state(&s), ZV_LINK_DEGRADED); /* stays degraded while retrying */
    ZV_CHECK_EQ(zv_link_on_connected(&s), ZV_OK);
    ZV_CHECK_EQ(zv_link_state(&s), ZV_LINK_ONLINE);
    ZV_CHECK(!poll(&s, 3800));
}

static void test_breaker_open_extends_wait(void)
{
    zv_link_supervisor_t s;
    ZV_CHECK_EQ(zv_link_init(&s, &k_cfg, rand_zero, NULL, 0), ZV_OK);
    uint32_t now = 0;
    for (int i = 0; i < 5; i++) {
        ZV_CHECK(poll(&s, now));
        ZV_CHECK_EQ(zv_link_on_failure(&s, now), ZV_OK);
        now += 10000u;
    }
    ZV_CHECK_EQ(s.breaker.state, ZV_CB_OPEN);
    ZV_CHECK_EQ(zv_link_state(&s), ZV_LINK_DEGRADED);
    uint32_t tripped_at = now - 10000u;
    ZV_CHECK_EQ(s.wait_ms, 30000);
    ZV_CHECK(!poll(&s, tripped_at + 29999u));
    ZV_CHECK(poll(&s, tripped_at + 30000u));
    ZV_CHECK_EQ(s.breaker.state, ZV_CB_HALF_OPEN);
}

static void test_planned_reconnect(void)
{
    zv_link_supervisor_t s;
    ZV_CHECK_EQ(zv_link_on_reconnect(NULL, 0u), ZV_ERR_INVALID_ARG);
    ZV_CHECK_EQ(zv_link_init(&s, &k_cfg, rand_zero, NULL, 0u), ZV_OK);
    ZV_CHECK_EQ(zv_link_on_reconnect(&s, 0u), ZV_ERR_INVALID_STATE);
    ZV_CHECK(poll(&s, 0u));
    ZV_CHECK_EQ(zv_link_on_connected(&s), ZV_OK);
    for (unsigned i = 0; i < 20u; i++) {
        ZV_CHECK_EQ(zv_link_on_reconnect(&s, UINT32_MAX), ZV_OK);
        ZV_CHECK_EQ(zv_link_state(&s), ZV_LINK_RECONNECTING);
        ZV_CHECK_STR(zv_link_state_name(zv_link_state(&s)), "cancelled");
        ZV_CHECK_EQ(s.wait_ms, 0u);
        ZV_CHECK_EQ(s.consecutive_failures, 0u);
        ZV_CHECK_EQ(s.breaker.trip_count, 0u);
        ZV_CHECK(poll(&s, 0u)); /* immediate, including timer wrap */
        ZV_CHECK_EQ(zv_link_state(&s), ZV_LINK_RECONNECTING);
        ZV_CHECK(!poll(&s, 1u));
        ZV_CHECK_EQ(zv_link_on_connected(&s), ZV_OK);
    }
    ZV_CHECK_EQ(zv_link_on_reconnect(&s, 5u), ZV_OK);
    ZV_CHECK(poll(&s, 5u));
    ZV_CHECK_EQ(zv_link_on_failure(&s, 6u), ZV_OK);
    ZV_CHECK_EQ(s.consecutive_failures, 1u);
    ZV_CHECK_EQ(s.wait_ms, 500u);
    ZV_CHECK(!poll(&s, 505u)); /* real failure still backs off */
    ZV_CHECK(poll(&s, 506u));
}

static void test_invalid_args(void)
{
    zv_link_supervisor_t s;
    zv_link_config_t bad = k_cfg;
    bad.degraded_after_failures = 0u;
    ZV_CHECK_EQ(zv_link_init(&s, &bad, rand_zero, NULL, 0), ZV_ERR_INVALID_ARG);
    ZV_CHECK_EQ(zv_link_init(&s, &k_cfg, NULL, NULL, 0), ZV_ERR_INVALID_ARG);
    ZV_CHECK_EQ(zv_link_poll(NULL, 0, NULL), ZV_ERR_INVALID_ARG);
    ZV_CHECK_STR(zv_link_state_name(ZV_LINK_DEGRADED), "degraded");
}

static void test_jitter_is_bounded_and_saturates(void)
{
    zv_link_supervisor_t low;
    zv_link_supervisor_t high;
    zv_link_config_t cfg = k_cfg;
    cfg.breaker.failure_threshold = 100u;
    ZV_CHECK_EQ(zv_link_init(&low, &cfg, rand_zero, NULL, 0u), ZV_OK);
    ZV_CHECK_EQ(zv_link_init(&high, &cfg, rand_max, NULL, 0u), ZV_OK);
    for (unsigned i = 0; i < 8u; i++) {
        ZV_CHECK(poll(&low, low.wait_start_ms + low.wait_ms));
        ZV_CHECK(poll(&high, high.wait_start_ms + high.wait_ms));
        ZV_CHECK_EQ(zv_link_on_failure(&low, low.wait_start_ms + low.wait_ms), ZV_OK);
        ZV_CHECK_EQ(zv_link_on_failure(&high, high.wait_start_ms + high.wait_ms), ZV_OK);
        ZV_CHECK(low.wait_ms >= 500u && low.wait_ms <= 8000u);
        ZV_CHECK(high.wait_ms >= 500u && high.wait_ms <= 8000u);
    }
}

static void test_millisecond_wraparound(void)
{
    zv_link_supervisor_t s;
    ZV_CHECK_EQ(zv_link_init(&s, &k_cfg, rand_zero, NULL, UINT32_MAX - 100u), ZV_OK);
    ZV_CHECK(poll(&s, UINT32_MAX - 100u));
    ZV_CHECK_EQ(zv_link_on_failure(&s, UINT32_MAX - 50u), ZV_OK);
    ZV_CHECK(!poll(&s, 200u));
    ZV_CHECK(poll(&s, 449u));
}

void test_suite_link_supervisor(void)
{
    test_reconnect_progression();
    test_breaker_open_extends_wait();
    test_invalid_args();
    test_planned_reconnect();
    test_jitter_is_bounded_and_saturates();
    test_millisecond_wraparound();
}
