/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 */
#include "zv_circuit_breaker.h"
#include "zv_test.h"

static const zv_cb_config_t k_cfg = {.failure_threshold = 3u, .open_duration_ms = 1000u, .half_open_max_trials = 1u};

static void test_trip_and_recover(void)
{
    zv_circuit_breaker_t cb;
    ZV_CHECK_EQ(zv_cb_init(&cb, &k_cfg), ZV_OK);
    ZV_CHECK_EQ(zv_cb_allow(&cb, 0), ZV_OK);
    zv_cb_record_failure(&cb, 10);
    zv_cb_record_failure(&cb, 20);
    ZV_CHECK_EQ(cb.state, ZV_CB_CLOSED);
    zv_cb_record_failure(&cb, 30);
    ZV_CHECK_EQ(cb.state, ZV_CB_OPEN);
    ZV_CHECK_EQ(cb.trip_count, 1);

    ZV_CHECK_EQ(zv_cb_allow(&cb, 530), ZV_ERR_CIRCUIT_OPEN);
    ZV_CHECK_EQ(zv_cb_remaining_open_ms(&cb, 530), 500);
    ZV_CHECK_EQ(zv_cb_allow(&cb, 1030), ZV_OK);
    ZV_CHECK_EQ(cb.state, ZV_CB_HALF_OPEN);
    ZV_CHECK_EQ(zv_cb_allow(&cb, 1031), ZV_ERR_CIRCUIT_OPEN);

    zv_cb_record_failure(&cb, 1100);
    ZV_CHECK_EQ(cb.state, ZV_CB_OPEN);
    ZV_CHECK_EQ(cb.trip_count, 2);

    ZV_CHECK_EQ(zv_cb_allow(&cb, 2100), ZV_OK);
    zv_cb_record_success(&cb);
    ZV_CHECK_EQ(cb.state, ZV_CB_CLOSED);
    ZV_CHECK_EQ(cb.consecutive_failures, 0);
    ZV_CHECK_EQ(zv_cb_remaining_open_ms(&cb, 2200), 0);
}

static void test_clock_wraparound(void)
{
    zv_circuit_breaker_t cb;
    ZV_CHECK_EQ(zv_cb_init(&cb, &k_cfg), ZV_OK);
    uint32_t near_wrap = UINT32_MAX - 100u;
    zv_cb_record_failure(&cb, near_wrap);
    zv_cb_record_failure(&cb, near_wrap);
    zv_cb_record_failure(&cb, near_wrap);
    ZV_CHECK_EQ(zv_cb_allow(&cb, 800u), ZV_ERR_CIRCUIT_OPEN); /* 901 ms elapsed */
    ZV_CHECK_EQ(zv_cb_allow(&cb, 900u), ZV_OK);               /* 1001 ms elapsed */
}

static void test_invalid_config(void)
{
    zv_circuit_breaker_t cb;
    zv_cb_config_t bad = k_cfg;
    bad.failure_threshold = 0u;
    ZV_CHECK_EQ(zv_cb_init(&cb, &bad), ZV_ERR_INVALID_ARG);
    bad = k_cfg;
    bad.open_duration_ms = 0u;
    ZV_CHECK_EQ(zv_cb_init(&cb, &bad), ZV_ERR_INVALID_ARG);
    ZV_CHECK_EQ(zv_cb_init(NULL, &k_cfg), ZV_ERR_INVALID_ARG);
    ZV_CHECK_EQ(zv_cb_allow(NULL, 0), ZV_ERR_INVALID_ARG);
}

void test_suite_circuit_breaker(void)
{
    test_trip_and_recover();
    test_clock_wraparound();
    test_invalid_config();
}
