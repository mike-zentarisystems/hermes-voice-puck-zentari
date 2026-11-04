/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Application event types carried on the event bus. */
typedef enum {
    ZV_EVT_NONE = 0,
    ZV_EVT_BOOT_COMPLETE,         /**< data.boot.provisioned */
    ZV_EVT_PROVISIONING_DONE,
    ZV_EVT_LINK_UP,               /**< Authenticated gateway session established */
    ZV_EVT_LINK_DOWN,
    ZV_EVT_LINK_DEGRADED,         /**< Repeated failures or circuit breaker open */
    ZV_EVT_WAKE,                  /**< Tap / push-to-talk / barge-in */
    ZV_EVT_LISTEN_END,            /**< End of user utterance */
    ZV_EVT_RESPONSE_AUDIO_START,
    ZV_EVT_RESPONSE_AUDIO_END,
    ZV_EVT_ACTION_EXECUTING,      /**< data.ref.id = action id */
    ZV_EVT_ACTION_SUCCEEDED,
    ZV_EVT_ACTION_FAILED,
    ZV_EVT_APPROVAL_REQUESTED,    /**< data.ref.id = local approval slot */
    ZV_EVT_APPROVAL_RESOLVED,     /**< data.approval */
    ZV_EVT_CANCEL,
    ZV_EVT_ERROR,                 /**< data.error.code */
    ZV_EVT_TIMEOUT,               /**< Dwell or watchdog timer for the current state */
    ZV_EVT_OTA_START,
    ZV_EVT_OTA_FAILED,
    ZV_EVT_AUDIO_LEVEL,           /**< Telemetry: data.audio.level_permille */
    ZV_EVT_VISEME,                /**< Telemetry: data.viseme */
    ZV_EVT_NOTIFICATION,          /**< data.ref.id = notification slot */
    ZV_EVT_COUNT
} zv_event_type_t;

typedef enum {
    ZV_APPROVAL_OUTCOME_APPROVED = 0,
    ZV_APPROVAL_OUTCOME_DENIED,
    ZV_APPROVAL_OUTCOME_EXPIRED,
} zv_approval_outcome_t;

typedef struct {
    zv_event_type_t type;
    uint32_t timestamp_ms;
    union {
        struct {
            bool provisioned;
        } boot;
        struct {
            uint16_t level_permille;
        } audio;
        struct {
            uint8_t viseme;
            uint32_t start_ms;
        } viseme;
        struct {
            int32_t code;
        } error;
        struct {
            uint32_t id;
        } ref;
        struct {
            uint32_t id;
            zv_approval_outcome_t outcome;
        } approval;
    } data;
} zv_event_t;

/**
 * Critical events change application state and must never be silently
 * dropped. Telemetry events (audio level, viseme) may be dropped under load.
 */
bool zv_event_is_critical(zv_event_type_t type);

/** Stable event name for logs and diagnostics. Never returns NULL. */
const char *zv_event_type_name(zv_event_type_t type);

#ifdef __cplusplus
}
#endif
