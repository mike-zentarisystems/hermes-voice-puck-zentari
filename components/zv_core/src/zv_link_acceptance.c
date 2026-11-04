/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 */
#include "zv_link_acceptance.h"

#include <limits.h>

bool zv_link_reply_frame_valid(uint8_t opcode, bool fin, int payload_len, int payload_offset,
                               int data_len, size_t destination_size)
{
    if (opcode != ZV_WS_OPCODE_TEXT || !fin || payload_offset != 0 || payload_len <= 0 || data_len <= 0) {
        return false;
    }
    if (payload_len != data_len || destination_size == 0u) {
        return false;
    }
    return (size_t)data_len < destination_size;
}

static bool drop_within(size_t baseline, size_t current, size_t maximum)
{
    return current >= baseline || baseline - current <= maximum;
}

bool zv_link_memory_stable(const zv_link_memory_snapshot_t *baseline,
                           const zv_link_memory_snapshot_t *current,
                           const zv_link_memory_limits_t *limits)
{
    if (baseline == NULL || current == NULL || limits == NULL) {
        return false;
    }
    return current->internal_free >= limits->internal_floor &&
           current->internal_min_free >= limits->internal_floor &&
           current->internal_largest >= limits->internal_largest_floor &&
           current->dma_free >= limits->dma_floor && current->dma_min_free >= limits->dma_floor &&
           current->dma_largest >= limits->dma_largest_floor &&
           drop_within(baseline->internal_free, current->internal_free, limits->max_internal_drop) &&
           drop_within(baseline->dma_free, current->dma_free, limits->max_dma_drop);
}

void zv_link_ui_record(zv_link_ui_evidence_t *evidence, zv_link_state_t state, bool rendered)
{
    if (evidence == NULL) {
        return;
    }
    if (!rendered) {
        if (evidence->failure_count < UINT32_MAX) {
            evidence->failure_count++;
        }
        return;
    }
    if (state >= ZV_LINK_OFFLINE && state <= ZV_LINK_RECONNECTING) {
        evidence->rendered_mask |= 1u << (uint32_t)state;
    }
}

bool zv_link_ui_complete(const zv_link_ui_evidence_t *evidence)
{
    return evidence != NULL && evidence->failure_count == 0u &&
           (evidence->rendered_mask & ZV_LINK_UI_REQUIRED_MASK) == ZV_LINK_UI_REQUIRED_MASK;
}
