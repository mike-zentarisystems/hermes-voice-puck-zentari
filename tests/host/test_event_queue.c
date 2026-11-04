/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 */
#include "zv_event_queue.h"
#include "zv_test.h"

static zv_event_t make(zv_event_type_t type, uint32_t ts)
{
    zv_event_t e;
    memset(&e, 0, sizeof(e));
    e.type = type;
    e.timestamp_ms = ts;
    return e;
}

static void test_init_and_fifo(void)
{
    zv_event_t storage[4];
    zv_event_queue_t q;
    ZV_CHECK_EQ(zv_event_queue_init(NULL, storage, 4), ZV_ERR_INVALID_ARG);
    ZV_CHECK_EQ(zv_event_queue_init(&q, NULL, 4), ZV_ERR_INVALID_ARG);
    ZV_CHECK_EQ(zv_event_queue_init(&q, storage, 0), ZV_ERR_INVALID_ARG);
    ZV_CHECK_EQ(zv_event_queue_init(&q, storage, 4), ZV_OK);

    zv_event_t out;
    ZV_CHECK_EQ(zv_event_queue_pop(&q, &out), ZV_ERR_QUEUE_EMPTY);
    for (uint32_t i = 0; i < 3; i++) {
        zv_event_t e = make(ZV_EVT_WAKE, i);
        ZV_CHECK_EQ(zv_event_queue_push(&q, &e), ZV_OK);
    }
    ZV_CHECK_EQ(zv_event_queue_count(&q), 3);
    for (uint32_t i = 0; i < 3; i++) {
        ZV_CHECK_EQ(zv_event_queue_pop(&q, &out), ZV_OK);
        ZV_CHECK_EQ(out.timestamp_ms, i);
    }
    ZV_CHECK_EQ(q.high_watermark, 3);

    zv_event_t bad = make(ZV_EVT_COUNT, 0);
    ZV_CHECK_EQ(zv_event_queue_push(&q, &bad), ZV_ERR_INVALID_ARG);
}

static void test_wraparound(void)
{
    zv_event_t storage[3];
    zv_event_queue_t q;
    ZV_CHECK_EQ(zv_event_queue_init(&q, storage, 3), ZV_OK);
    zv_event_t out;
    for (uint32_t i = 0; i < 20; i++) {
        zv_event_t e = make(ZV_EVT_TIMEOUT, i);
        ZV_CHECK_EQ(zv_event_queue_push(&q, &e), ZV_OK);
        ZV_CHECK_EQ(zv_event_queue_pop(&q, &out), ZV_OK);
        ZV_CHECK_EQ(out.timestamp_ms, i);
    }
    ZV_CHECK_EQ(zv_event_queue_count(&q), 0);
}

static void test_overflow_policy(void)
{
    zv_event_t storage[3];
    zv_event_queue_t q;
    ZV_CHECK_EQ(zv_event_queue_init(&q, storage, 3), ZV_OK);

    zv_event_t level0 = make(ZV_EVT_AUDIO_LEVEL, 0);
    zv_event_t wake1 = make(ZV_EVT_WAKE, 1);
    zv_event_t level2 = make(ZV_EVT_AUDIO_LEVEL, 2);
    ZV_CHECK_EQ(zv_event_queue_push(&q, &level0), ZV_OK);
    ZV_CHECK_EQ(zv_event_queue_push(&q, &wake1), ZV_OK);
    ZV_CHECK_EQ(zv_event_queue_push(&q, &level2), ZV_OK);

    /* Telemetry into a full queue is dropped. */
    zv_event_t viseme = make(ZV_EVT_VISEME, 3);
    ZV_CHECK_EQ(zv_event_queue_push(&q, &viseme), ZV_ERR_QUEUE_FULL);
    ZV_CHECK_EQ(q.dropped, 1);

    /* Critical event evicts the oldest telemetry (ts 0) and keeps order. */
    zv_event_t end4 = make(ZV_EVT_LISTEN_END, 4);
    ZV_CHECK_EQ(zv_event_queue_push(&q, &end4), ZV_OK);
    ZV_CHECK_EQ(q.dropped, 2);
    zv_event_t out;
    ZV_CHECK_EQ(zv_event_queue_pop(&q, &out), ZV_OK);
    ZV_CHECK_EQ(out.timestamp_ms, 1);
    ZV_CHECK_EQ(zv_event_queue_pop(&q, &out), ZV_OK);
    ZV_CHECK_EQ(out.timestamp_ms, 2);
    ZV_CHECK_EQ(zv_event_queue_pop(&q, &out), ZV_OK);
    ZV_CHECK_EQ(out.timestamp_ms, 4);

    /* Queue full of critical events rejects another critical event loudly. */
    for (uint32_t i = 0; i < 3; i++) {
        zv_event_t e = make(ZV_EVT_ERROR, 10 + i);
        ZV_CHECK_EQ(zv_event_queue_push(&q, &e), ZV_OK);
    }
    zv_event_t extra = make(ZV_EVT_CANCEL, 99);
    ZV_CHECK_EQ(zv_event_queue_push(&q, &extra), ZV_ERR_QUEUE_FULL);
    ZV_CHECK_EQ(q.rejected_critical, 1);
    ZV_CHECK_EQ(zv_event_queue_pop(&q, &out), ZV_OK);
    ZV_CHECK_EQ(out.timestamp_ms, 10);
}

void test_suite_event_queue(void)
{
    test_init_and_fifo();
    test_wraparound();
    test_overflow_policy();
}
