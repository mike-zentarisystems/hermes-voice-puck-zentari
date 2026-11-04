/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 */
#include "zv_backoff.h"
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

static uint32_t rand_counter(void *ctx)
{
    uint32_t *counter = (uint32_t *)ctx;
    *counter = *counter * 1664525u + 1013904223u;
    return *counter;
}

static void test_init_validation(void)
{
    zv_backoff_t b;
    ZV_CHECK_EQ(zv_backoff_init(NULL, 100, 1000, rand_zero, NULL), ZV_ERR_INVALID_ARG);
    ZV_CHECK_EQ(zv_backoff_init(&b, 0, 1000, rand_zero, NULL), ZV_ERR_INVALID_ARG);
    ZV_CHECK_EQ(zv_backoff_init(&b, 2000, 1000, rand_zero, NULL), ZV_ERR_INVALID_ARG);
    ZV_CHECK_EQ(zv_backoff_init(&b, 100, 1000, NULL, NULL), ZV_ERR_INVALID_ARG);
    uint32_t d = 0;
    ZV_CHECK_EQ(zv_backoff_next(NULL, &d), ZV_ERR_INVALID_ARG);
}

static void test_lower_bound_sequence(void)
{
    zv_backoff_t b;
    ZV_CHECK_EQ(zv_backoff_init(&b, 500, 30000, rand_zero, NULL), ZV_OK);
    const uint32_t expected[] = {250, 500, 1000, 2000, 4000, 8000, 15000, 15000};
    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
        uint32_t d = 0;
        ZV_CHECK_EQ(zv_backoff_next(&b, &d), ZV_OK);
        ZV_CHECK_EQ(d, expected[i]);
    }
    zv_backoff_reset(&b);
    uint32_t d = 0;
    ZV_CHECK_EQ(zv_backoff_next(&b, &d), ZV_OK);
    ZV_CHECK_EQ(d, 250);
}

static void test_bounds_and_saturation(void)
{
    zv_backoff_t b;
    ZV_CHECK_EQ(zv_backoff_init(&b, 1000, 60000, rand_max, NULL), ZV_OK);
    for (int i = 0; i < 200; i++) {
        uint32_t d = 0;
        ZV_CHECK_EQ(zv_backoff_next(&b, &d), ZV_OK);
        ZV_CHECK(d <= 60000u);
    }

    uint32_t seed = 42u;
    ZV_CHECK_EQ(zv_backoff_init(&b, 1000, 60000, rand_counter, &seed), ZV_OK);
    uint32_t ceiling = 1000u;
    for (int i = 0; i < 12; i++) {
        uint32_t d = 0;
        ZV_CHECK_EQ(zv_backoff_next(&b, &d), ZV_OK);
        ZV_CHECK(d >= ceiling / 2u);
        ZV_CHECK(d <= ceiling);
        ceiling = (ceiling * 2u > 60000u) ? 60000u : ceiling * 2u;
    }
}

void test_suite_backoff(void)
{
    test_init_validation();
    test_lower_bound_sequence();
    test_bounds_and_saturation();
}
