/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 */
#include "zv_link_acceptance.h"
#include "zv_test.h"

static void test_reply_requires_one_complete_text_frame(void)
{
    ZV_CHECK(zv_link_reply_frame_valid(ZV_WS_OPCODE_TEXT, true, 12, 0, 12, 64u));
    ZV_CHECK(!zv_link_reply_frame_valid(0x02u, true, 12, 0, 12, 64u));
    ZV_CHECK(!zv_link_reply_frame_valid(ZV_WS_OPCODE_TEXT, false, 12, 0, 12, 64u));
    ZV_CHECK(!zv_link_reply_frame_valid(ZV_WS_OPCODE_TEXT, true, 12, 4, 8, 64u));
    ZV_CHECK(!zv_link_reply_frame_valid(ZV_WS_OPCODE_TEXT, true, 12, 0, 8, 64u));
    ZV_CHECK(!zv_link_reply_frame_valid(ZV_WS_OPCODE_TEXT, true, 64, 0, 64, 64u));
    ZV_CHECK(!zv_link_reply_frame_valid(ZV_WS_OPCODE_TEXT, true, 0, 0, 0, 64u));
}

static void test_memory_gate_enforces_floors_and_drift(void)
{
    const zv_link_memory_snapshot_t baseline = {200000u, 190000u, 100000u, 150000u, 140000u, 80000u};
    zv_link_memory_snapshot_t current = {190000u, 180000u, 90000u, 142000u, 135000u, 70000u};
    const zv_link_memory_limits_t limits = {65536u, 32768u, 65536u, 16384u, 32768u, 32768u};
    ZV_CHECK(zv_link_memory_stable(&baseline, &current, &limits));
    current.internal_free = 60000u;
    ZV_CHECK(!zv_link_memory_stable(&baseline, &current, &limits));
    current = baseline;
    current.dma_free = 100000u;
    ZV_CHECK(!zv_link_memory_stable(&baseline, &current, &limits));
}

static void test_ui_gate_requires_every_state_without_errors(void)
{
    zv_link_ui_evidence_t evidence = {0};
    zv_link_ui_record(&evidence, ZV_LINK_OFFLINE, true);
    zv_link_ui_record(&evidence, ZV_LINK_CONNECTING, true);
    zv_link_ui_record(&evidence, ZV_LINK_DEGRADED, true);
    ZV_CHECK(!zv_link_ui_complete(&evidence));
    zv_link_ui_record(&evidence, ZV_LINK_ONLINE, true);
    ZV_CHECK(zv_link_ui_complete(&evidence));
    zv_link_ui_record(&evidence, ZV_LINK_ONLINE, false);
    ZV_CHECK(!zv_link_ui_complete(&evidence));
}

void test_suite_link_acceptance(void)
{
    test_reply_requires_one_complete_text_frame();
    test_memory_gate_enforces_floors_and_drift();
    test_ui_gate_requires_every_state_without_errors();
}
