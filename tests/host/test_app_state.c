/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 */
#include "zv_app_state.h"
#include "zv_test.h"

static zv_event_t evt(zv_event_type_t type)
{
    zv_event_t e;
    memset(&e, 0, sizeof(e));
    e.type = type;
    return e;
}

static zv_app_state_t step(zv_app_state_t current, zv_event_t e, zv_status_t expected_status)
{
    zv_app_state_t next = ZV_STATE_COUNT;
    ZV_CHECK_EQ(zv_app_state_next(current, &e, &next), expected_status);
    return next;
}

static void test_boot_paths(void)
{
    zv_event_t boot = evt(ZV_EVT_BOOT_COMPLETE);
    boot.data.boot.provisioned = true;
    ZV_CHECK_EQ(step(ZV_STATE_BOOT, boot, ZV_OK), ZV_STATE_CONNECTING);
    boot.data.boot.provisioned = false;
    ZV_CHECK_EQ(step(ZV_STATE_BOOT, boot, ZV_OK), ZV_STATE_PROVISIONING);
    ZV_CHECK_EQ(step(ZV_STATE_PROVISIONING, evt(ZV_EVT_PROVISIONING_DONE), ZV_OK), ZV_STATE_CONNECTING);
}

static void test_conversation_happy_path(void)
{
    zv_app_state_t s = step(ZV_STATE_CONNECTING, evt(ZV_EVT_LINK_UP), ZV_OK);
    ZV_CHECK_EQ(s, ZV_STATE_IDLE);
    s = step(s, evt(ZV_EVT_WAKE), ZV_OK);
    ZV_CHECK_EQ(s, ZV_STATE_LISTENING);
    s = step(s, evt(ZV_EVT_LISTEN_END), ZV_OK);
    ZV_CHECK_EQ(s, ZV_STATE_THINKING);
    s = step(s, evt(ZV_EVT_RESPONSE_AUDIO_START), ZV_OK);
    ZV_CHECK_EQ(s, ZV_STATE_SPEAKING);
    s = step(s, evt(ZV_EVT_AUDIO_LEVEL), ZV_OK);
    ZV_CHECK_EQ(s, ZV_STATE_SPEAKING);
    s = step(s, evt(ZV_EVT_WAKE), ZV_OK); /* barge-in */
    ZV_CHECK_EQ(s, ZV_STATE_LISTENING);
    s = step(s, evt(ZV_EVT_CANCEL), ZV_OK);
    ZV_CHECK_EQ(s, ZV_STATE_IDLE);
}

static void test_approval_paths(void)
{
    zv_event_t resolved = evt(ZV_EVT_APPROVAL_RESOLVED);
    resolved.data.approval.outcome = ZV_APPROVAL_OUTCOME_APPROVED;
    zv_app_state_t s = step(ZV_STATE_THINKING, evt(ZV_EVT_APPROVAL_REQUESTED), ZV_OK);
    ZV_CHECK_EQ(s, ZV_STATE_APPROVAL);
    s = step(s, resolved, ZV_OK);
    ZV_CHECK_EQ(s, ZV_STATE_EXECUTING);
    s = step(s, evt(ZV_EVT_ACTION_SUCCEEDED), ZV_OK);
    ZV_CHECK_EQ(s, ZV_STATE_SUCCESS);
    s = step(s, evt(ZV_EVT_TIMEOUT), ZV_OK);
    ZV_CHECK_EQ(s, ZV_STATE_IDLE);

    resolved.data.approval.outcome = ZV_APPROVAL_OUTCOME_DENIED;
    ZV_CHECK_EQ(step(ZV_STATE_APPROVAL, resolved, ZV_OK), ZV_STATE_IDLE);
    resolved.data.approval.outcome = ZV_APPROVAL_OUTCOME_EXPIRED;
    ZV_CHECK_EQ(step(ZV_STATE_APPROVAL, resolved, ZV_OK), ZV_STATE_IDLE);

    /* Losing the link while an approval is pending must fail closed. */
    ZV_CHECK_EQ(step(ZV_STATE_APPROVAL, evt(ZV_EVT_LINK_DOWN), ZV_OK), ZV_STATE_DEGRADED);
    ZV_CHECK_EQ(step(ZV_STATE_EXECUTING, evt(ZV_EVT_ACTION_FAILED), ZV_OK), ZV_STATE_ERROR);
}

static void test_invalid_and_global(void)
{
    ZV_CHECK_EQ(step(ZV_STATE_IDLE, evt(ZV_EVT_LISTEN_END), ZV_ERR_INVALID_TRANSITION), ZV_STATE_IDLE);
    ZV_CHECK_EQ(step(ZV_STATE_DEGRADED, evt(ZV_EVT_WAKE), ZV_ERR_INVALID_TRANSITION), ZV_STATE_DEGRADED);
    ZV_CHECK_EQ(step(ZV_STATE_EXECUTING, evt(ZV_EVT_CANCEL), ZV_ERR_INVALID_TRANSITION), ZV_STATE_EXECUTING);
    ZV_CHECK_EQ(step(ZV_STATE_UPDATING, evt(ZV_EVT_LINK_DOWN), ZV_ERR_INVALID_TRANSITION), ZV_STATE_UPDATING);
    ZV_CHECK_EQ(step(ZV_STATE_DEGRADED, evt(ZV_EVT_LINK_UP), ZV_OK), ZV_STATE_IDLE);
    ZV_CHECK_EQ(step(ZV_STATE_IDLE, evt(ZV_EVT_OTA_START), ZV_OK), ZV_STATE_UPDATING);
    ZV_CHECK_EQ(step(ZV_STATE_UPDATING, evt(ZV_EVT_OTA_FAILED), ZV_OK), ZV_STATE_ERROR);

    for (int s = 0; s < (int)ZV_STATE_COUNT; s++) {
        ZV_CHECK_EQ(step((zv_app_state_t)s, evt(ZV_EVT_ERROR), ZV_OK), ZV_STATE_ERROR);
        ZV_CHECK_EQ(step((zv_app_state_t)s, evt(ZV_EVT_VISEME), ZV_OK), s);
    }

    zv_app_state_t next = ZV_STATE_IDLE;
    zv_event_t e = evt(ZV_EVT_WAKE);
    ZV_CHECK_EQ(zv_app_state_next(ZV_STATE_IDLE, NULL, &next), ZV_ERR_INVALID_ARG);
    ZV_CHECK_EQ(zv_app_state_next(ZV_STATE_IDLE, &e, NULL), ZV_ERR_INVALID_ARG);
    ZV_CHECK_EQ(zv_app_state_next(ZV_STATE_COUNT, &e, &next), ZV_ERR_INVALID_ARG);
    e.type = ZV_EVT_COUNT;
    ZV_CHECK_EQ(zv_app_state_next(ZV_STATE_IDLE, &e, &next), ZV_ERR_INVALID_ARG);
}

static void test_names_and_avatar_mapping(void)
{
    int seen[ZV_AVATAR_STATE_COUNT] = {0};
    for (int s = 0; s < (int)ZV_STATE_COUNT; s++) {
        ZV_CHECK(strcmp(zv_app_state_name((zv_app_state_t)s), "unknown") != 0);
        zv_avatar_state_t a = zv_app_state_to_avatar((zv_app_state_t)s);
        ZV_CHECK((int)a < (int)ZV_AVATAR_STATE_COUNT);
        seen[a] = 1;
    }
    for (int a = 0; a < (int)ZV_AVATAR_STATE_COUNT; a++) {
        ZV_CHECK_EQ(seen[a], 1);
        for (int b = a + 1; b < (int)ZV_AVATAR_STATE_COUNT; b++) {
            ZV_CHECK(strcmp(zv_avatar_state_name((zv_avatar_state_t)a), zv_avatar_state_name((zv_avatar_state_t)b)) != 0);
        }
    }
    ZV_CHECK_STR(zv_avatar_state_name(ZV_AVATAR_APPROVAL), "approval");
    ZV_CHECK_STR(zv_avatar_state_name(ZV_AVATAR_STATE_COUNT), "unknown");
}

void test_suite_app_state(void)
{
    test_boot_paths();
    test_conversation_happy_path();
    test_approval_paths();
    test_invalid_and_global();
    test_names_and_avatar_mapping();
}
