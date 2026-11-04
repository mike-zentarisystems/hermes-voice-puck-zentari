/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 */
#include "zv_event_queue.h"

#include <stddef.h>
#include <stdbool.h>

static size_t slot_index(const zv_event_queue_t *queue, size_t offset)
{
    return (queue->head + offset) % queue->capacity;
}

zv_status_t zv_event_queue_init(zv_event_queue_t *queue, zv_event_t *storage, size_t capacity)
{
    if (queue == NULL || storage == NULL || capacity == 0u) {
        return ZV_ERR_INVALID_ARG;
    }
    queue->storage = storage;
    queue->capacity = capacity;
    queue->head = 0u;
    queue->count = 0u;
    queue->dropped = 0u;
    queue->rejected_critical = 0u;
    queue->high_watermark = 0u;
    return ZV_OK;
}

static bool evict_oldest_telemetry(zv_event_queue_t *queue)
{
    for (size_t i = 0u; i < queue->count; i++) {
        if (zv_event_is_critical(queue->storage[slot_index(queue, i)].type)) {
            continue;
        }
        for (size_t j = i; j + 1u < queue->count; j++) {
            queue->storage[slot_index(queue, j)] = queue->storage[slot_index(queue, j + 1u)];
        }
        queue->count--;
        queue->dropped++;
        return true;
    }
    return false;
}

zv_status_t zv_event_queue_push(zv_event_queue_t *queue, const zv_event_t *event)
{
    if (queue == NULL || event == NULL || queue->storage == NULL) {
        return ZV_ERR_INVALID_ARG;
    }
    if ((unsigned)event->type >= (unsigned)ZV_EVT_COUNT) {
        return ZV_ERR_INVALID_ARG;
    }
    if (queue->count == queue->capacity) {
        if (!zv_event_is_critical(event->type)) {
            queue->dropped++;
            return ZV_ERR_QUEUE_FULL;
        }
        if (!evict_oldest_telemetry(queue)) {
            queue->rejected_critical++;
            return ZV_ERR_QUEUE_FULL;
        }
    }
    queue->storage[slot_index(queue, queue->count)] = *event;
    queue->count++;
    if (queue->count > queue->high_watermark) {
        queue->high_watermark = (uint32_t)queue->count;
    }
    return ZV_OK;
}

zv_status_t zv_event_queue_pop(zv_event_queue_t *queue, zv_event_t *out)
{
    if (queue == NULL || out == NULL || queue->storage == NULL) {
        return ZV_ERR_INVALID_ARG;
    }
    if (queue->count == 0u) {
        return ZV_ERR_QUEUE_EMPTY;
    }
    *out = queue->storage[queue->head];
    queue->head = (queue->head + 1u) % queue->capacity;
    queue->count--;
    return ZV_OK;
}

size_t zv_event_queue_count(const zv_event_queue_t *queue)
{
    return queue == NULL ? 0u : queue->count;
}
