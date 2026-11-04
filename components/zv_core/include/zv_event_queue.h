/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "zv_event.h"
#include "zv_status.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Bounded, allocation-free FIFO of events with priority-aware overflow:
 * - telemetry pushed into a full queue is dropped (counted in `dropped`);
 * - a critical event pushed into a full queue evicts the oldest telemetry
 *   event; if none exists the push fails with ZV_ERR_QUEUE_FULL and
 *   `rejected_critical` is incremented so the caller can raise an error.
 * Not thread-safe; the firmware bus wraps it in a critical section.
 */
typedef struct {
    zv_event_t *storage;
    size_t capacity;
    size_t head;
    size_t count;
    uint32_t dropped;
    uint32_t rejected_critical;
    uint32_t high_watermark;
} zv_event_queue_t;

zv_status_t zv_event_queue_init(zv_event_queue_t *queue, zv_event_t *storage, size_t capacity);
zv_status_t zv_event_queue_push(zv_event_queue_t *queue, const zv_event_t *event);
zv_status_t zv_event_queue_pop(zv_event_queue_t *queue, zv_event_t *out);
size_t zv_event_queue_count(const zv_event_queue_t *queue);

#ifdef __cplusplus
}
#endif
