/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 */
#include "zv_cred_rotation.h"
#include "zv_test.h"

static void test_happy_path(void)
{
    zv_rotation_t rot;
    ZV_CHECK_EQ(zv_rotation_init(&rot, 3u), ZV_OK);
    ZV_CHECK_EQ(zv_rotation_stage(&rot, 4u, 15000u), ZV_OK);
    ZV_CHECK_EQ(rot.state, ZV_ROT_STAGED);
    ZV_CHECK_EQ(zv_rotation_stage(&rot, 5u, 15000u), ZV_ERR_INVALID_STATE);
    ZV_CHECK_EQ(zv_rotation_begin_verify(&rot, 1000u), ZV_OK);
    ZV_CHECK_EQ(zv_rotation_tick(&rot, 5000u), ZV_ROT_VERIFYING);
    ZV_CHECK_EQ(zv_rotation_on_verified(&rot), ZV_OK);
    ZV_CHECK_EQ(rot.state, ZV_ROT_COMMITTED);
    ZV_CHECK_EQ(rot.active_generation, 4u);
    ZV_CHECK_EQ(rot.staged_generation, 0u);
    ZV_CHECK_EQ(zv_rotation_stage(&rot, 5u, 15000u), ZV_OK);
}

static void test_rejections_and_rollback(void)
{
    zv_rotation_t rot;
    ZV_CHECK_EQ(zv_rotation_init(&rot, 7u), ZV_OK);
    ZV_CHECK_EQ(zv_rotation_stage(&rot, 7u, 15000u), ZV_ERR_POLICY); /* replayed generation */
    ZV_CHECK_EQ(zv_rotation_stage(&rot, 6u, 15000u), ZV_ERR_POLICY);
    ZV_CHECK_EQ(zv_rotation_stage(&rot, 8u, 0u), ZV_ERR_INVALID_ARG);
    ZV_CHECK_EQ(zv_rotation_on_verified(&rot), ZV_ERR_INVALID_STATE);
    ZV_CHECK_EQ(zv_rotation_begin_verify(&rot, 0u), ZV_ERR_INVALID_STATE);

    ZV_CHECK_EQ(zv_rotation_stage(&rot, 8u, 15000u), ZV_OK);
    ZV_CHECK_EQ(zv_rotation_begin_verify(&rot, 1000u), ZV_OK);
    ZV_CHECK_EQ(zv_rotation_tick(&rot, 16000u), ZV_ROT_ROLLED_BACK);
    ZV_CHECK_EQ(rot.active_generation, 7u);

    ZV_CHECK_EQ(zv_rotation_stage(&rot, 9u, 15000u), ZV_OK);
    ZV_CHECK_EQ(zv_rotation_begin_verify(&rot, 20000u), ZV_OK);
    ZV_CHECK_EQ(zv_rotation_on_failed(&rot), ZV_OK);
    ZV_CHECK_EQ(rot.state, ZV_ROT_ROLLED_BACK);
    ZV_CHECK_EQ(rot.active_generation, 7u);
    ZV_CHECK_EQ(zv_rotation_on_failed(&rot), ZV_ERR_INVALID_STATE);
    ZV_CHECK_EQ(zv_rotation_init(NULL, 0u), ZV_ERR_INVALID_ARG);
}

void test_suite_cred_rotation(void)
{
    test_happy_path();
    test_rejections_and_rollback();
}
