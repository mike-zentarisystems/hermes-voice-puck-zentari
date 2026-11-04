/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "zv_link_supervisor.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZV_WS_OPCODE_TEXT 0x01u
#define ZV_LINK_UI_REQUIRED_MASK ((1u << ZV_LINK_OFFLINE) | (1u << ZV_LINK_CONNECTING) | \
                                  (1u << ZV_LINK_ONLINE) | (1u << ZV_LINK_DEGRADED))

typedef struct {
    size_t internal_free;
    size_t internal_min_free;
    size_t internal_largest;
    size_t dma_free;
    size_t dma_min_free;
    size_t dma_largest;
} zv_link_memory_snapshot_t;

typedef struct {
    size_t internal_floor;
    size_t internal_largest_floor;
    size_t dma_floor;
    size_t dma_largest_floor;
    size_t max_internal_drop;
    size_t max_dma_drop;
} zv_link_memory_limits_t;

typedef struct {
    uint32_t rendered_mask;
    uint32_t failure_count;
} zv_link_ui_evidence_t;

bool zv_link_reply_frame_valid(uint8_t opcode, bool fin, int payload_len, int payload_offset,
                               int data_len, size_t destination_size);
bool zv_link_memory_stable(const zv_link_memory_snapshot_t *baseline,
                           const zv_link_memory_snapshot_t *current,
                           const zv_link_memory_limits_t *limits);
void zv_link_ui_record(zv_link_ui_evidence_t *evidence, zv_link_state_t state, bool rendered);
bool zv_link_ui_complete(const zv_link_ui_evidence_t *evidence);

#ifdef __cplusplus
}
#endif
