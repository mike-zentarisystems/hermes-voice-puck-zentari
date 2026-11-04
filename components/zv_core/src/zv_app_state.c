/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stddef.h>

#include "zv_app_state.h"

#include <stdbool.h>

static bool is_telemetry(zv_event_type_t type)
{
    return type == ZV_EVT_NONE || type == ZV_EVT_AUDIO_LEVEL || type == ZV_EVT_VISEME ||
           type == ZV_EVT_NOTIFICATION;
}

/** States that require a live gateway session and degrade when it is lost. */
static bool depends_on_link(zv_app_state_t state)
{
    switch (state) {
    case ZV_STATE_CONNECTING:
    case ZV_STATE_IDLE:
    case ZV_STATE_LISTENING:
    case ZV_STATE_THINKING:
    case ZV_STATE_SPEAKING:
    case ZV_STATE_EXECUTING:
    case ZV_STATE_APPROVAL: /* fail closed: a pending approval cannot be answered offline */
    case ZV_STATE_SUCCESS:
    case ZV_STATE_ERROR:
        return true;
    default:
        return false;
    }
}

static bool per_state_rule(zv_app_state_t current, const zv_event_t *event, zv_app_state_t *next)
{
    zv_event_type_t t = event->type;

    switch (current) {
    case ZV_STATE_BOOT:
        if (t == ZV_EVT_BOOT_COMPLETE) {
            *next = event->data.boot.provisioned ? ZV_STATE_CONNECTING : ZV_STATE_PROVISIONING;
            return true;
        }
        return false;
    case ZV_STATE_PROVISIONING:
        if (t == ZV_EVT_PROVISIONING_DONE) {
            *next = ZV_STATE_CONNECTING;
            return true;
        }
        return false;
    case ZV_STATE_CONNECTING:
    case ZV_STATE_DEGRADED:
        if (t == ZV_EVT_LINK_UP) {
            *next = ZV_STATE_IDLE;
            return true;
        }
        if (current == ZV_STATE_DEGRADED && t == ZV_EVT_OTA_START) {
            *next = ZV_STATE_UPDATING;
            return true;
        }
        return false;
    case ZV_STATE_IDLE:
        switch (t) {
        case ZV_EVT_WAKE:
            *next = ZV_STATE_LISTENING;
            return true;
        case ZV_EVT_APPROVAL_REQUESTED:
            *next = ZV_STATE_APPROVAL;
            return true;
        case ZV_EVT_ACTION_EXECUTING:
            *next = ZV_STATE_EXECUTING;
            return true;
        case ZV_EVT_RESPONSE_AUDIO_START:
            *next = ZV_STATE_SPEAKING;
            return true;
        case ZV_EVT_OTA_START:
            *next = ZV_STATE_UPDATING;
            return true;
        default:
            return false;
        }
    case ZV_STATE_LISTENING:
        if (t == ZV_EVT_LISTEN_END) {
            *next = ZV_STATE_THINKING;
            return true;
        }
        if (t == ZV_EVT_CANCEL || t == ZV_EVT_TIMEOUT) {
            *next = ZV_STATE_IDLE;
            return true;
        }
        return false;
    case ZV_STATE_THINKING:
        switch (t) {
        case ZV_EVT_RESPONSE_AUDIO_START:
            *next = ZV_STATE_SPEAKING;
            return true;
        case ZV_EVT_ACTION_EXECUTING:
            *next = ZV_STATE_EXECUTING;
            return true;
        case ZV_EVT_APPROVAL_REQUESTED:
            *next = ZV_STATE_APPROVAL;
            return true;
        case ZV_EVT_TIMEOUT:
            *next = ZV_STATE_ERROR;
            return true;
        case ZV_EVT_CANCEL:
            *next = ZV_STATE_IDLE;
            return true;
        default:
            return false;
        }
    case ZV_STATE_SPEAKING:
        switch (t) {
        case ZV_EVT_RESPONSE_AUDIO_END:
        case ZV_EVT_CANCEL:
            *next = ZV_STATE_IDLE;
            return true;
        case ZV_EVT_WAKE: /* barge-in */
            *next = ZV_STATE_LISTENING;
            return true;
        case ZV_EVT_APPROVAL_REQUESTED:
            *next = ZV_STATE_APPROVAL;
            return true;
        case ZV_EVT_ACTION_EXECUTING:
            *next = ZV_STATE_EXECUTING;
            return true;
        default:
            return false;
        }
    case ZV_STATE_EXECUTING:
        switch (t) {
        case ZV_EVT_ACTION_SUCCEEDED:
            *next = ZV_STATE_SUCCESS;
            return true;
        case ZV_EVT_ACTION_FAILED:
            *next = ZV_STATE_ERROR;
            return true;
        case ZV_EVT_APPROVAL_REQUESTED:
            *next = ZV_STATE_APPROVAL;
            return true;
        case ZV_EVT_RESPONSE_AUDIO_START:
            *next = ZV_STATE_SPEAKING;
            return true;
        default:
            return false;
        }
    case ZV_STATE_APPROVAL:
        if (t == ZV_EVT_APPROVAL_RESOLVED) {
            *next = (event->data.approval.outcome == ZV_APPROVAL_OUTCOME_APPROVED) ? ZV_STATE_EXECUTING
                                                                                  : ZV_STATE_IDLE;
            return true;
        }
        if (t == ZV_EVT_CANCEL || t == ZV_EVT_TIMEOUT) {
            *next = ZV_STATE_IDLE;
            return true;
        }
        return false;
    case ZV_STATE_SUCCESS:
    case ZV_STATE_ERROR:
        switch (t) {
        case ZV_EVT_TIMEOUT:
        case ZV_EVT_LINK_UP:
        case ZV_EVT_CANCEL:
            *next = ZV_STATE_IDLE;
            return true;
        case ZV_EVT_WAKE:
            *next = ZV_STATE_LISTENING;
            return true;
        case ZV_EVT_RESPONSE_AUDIO_START:
            *next = ZV_STATE_SPEAKING;
            return true;
        case ZV_EVT_APPROVAL_REQUESTED:
            *next = ZV_STATE_APPROVAL;
            return true;
        default:
            return false;
        }
    case ZV_STATE_UPDATING:
        if (t == ZV_EVT_OTA_FAILED) {
            *next = ZV_STATE_ERROR;
            return true;
        }
        return false;
    case ZV_STATE_COUNT:
        break;
    }
    return false;
}

zv_status_t zv_app_state_next(zv_app_state_t current, const zv_event_t *event, zv_app_state_t *next)
{
    if (event == NULL || next == NULL || (unsigned)current >= (unsigned)ZV_STATE_COUNT ||
        (unsigned)event->type >= (unsigned)ZV_EVT_COUNT) {
        return ZV_ERR_INVALID_ARG;
    }
    *next = current;

    if (is_telemetry(event->type)) {
        return ZV_OK;
    }
    if (event->type == ZV_EVT_ERROR) {
        *next = ZV_STATE_ERROR;
        return ZV_OK;
    }
    if ((event->type == ZV_EVT_LINK_DOWN || event->type == ZV_EVT_LINK_DEGRADED) && depends_on_link(current)) {
        *next = ZV_STATE_DEGRADED;
        return ZV_OK;
    }
    if (per_state_rule(current, event, next)) {
        return ZV_OK;
    }
    *next = current;
    return ZV_ERR_INVALID_TRANSITION;
}

zv_avatar_state_t zv_app_state_to_avatar(zv_app_state_t state)
{
    switch (state) {
    case ZV_STATE_LISTENING:
        return ZV_AVATAR_LISTENING;
    case ZV_STATE_CONNECTING:
    case ZV_STATE_THINKING:
        return ZV_AVATAR_THINKING;
    case ZV_STATE_SPEAKING:
        return ZV_AVATAR_SPEAKING;
    case ZV_STATE_EXECUTING:
    case ZV_STATE_UPDATING:
        return ZV_AVATAR_EXECUTING;
    case ZV_STATE_SUCCESS:
        return ZV_AVATAR_SUCCESS;
    case ZV_STATE_ERROR:
        return ZV_AVATAR_ERROR;
    case ZV_STATE_APPROVAL:
        return ZV_AVATAR_APPROVAL;
    default:
        return ZV_AVATAR_IDLE;
    }
}

static const char *const k_state_names[ZV_STATE_COUNT] = {
    [ZV_STATE_BOOT] = "boot",           [ZV_STATE_PROVISIONING] = "provisioning",
    [ZV_STATE_CONNECTING] = "connecting", [ZV_STATE_IDLE] = "idle",
    [ZV_STATE_LISTENING] = "listening", [ZV_STATE_THINKING] = "thinking",
    [ZV_STATE_SPEAKING] = "speaking",   [ZV_STATE_EXECUTING] = "executing",
    [ZV_STATE_APPROVAL] = "approval",   [ZV_STATE_SUCCESS] = "success",
    [ZV_STATE_ERROR] = "error",         [ZV_STATE_DEGRADED] = "degraded",
    [ZV_STATE_UPDATING] = "updating",
};

/* Wire names; keep in sync with protocol/v1 and prototype/avatar.js (enforced by CI). */
static const char *const k_avatar_state_names[ZV_AVATAR_STATE_COUNT] = {
    [ZV_AVATAR_IDLE] = "idle",           [ZV_AVATAR_LISTENING] = "listening",
    [ZV_AVATAR_THINKING] = "thinking",   [ZV_AVATAR_SPEAKING] = "speaking",
    [ZV_AVATAR_EXECUTING] = "executing", [ZV_AVATAR_SUCCESS] = "success",
    [ZV_AVATAR_ERROR] = "error",         [ZV_AVATAR_APPROVAL] = "approval",
};

const char *zv_app_state_name(zv_app_state_t state)
{
    if ((unsigned)state >= (unsigned)ZV_STATE_COUNT) {
        return "unknown";
    }
    return k_state_names[state];
}

const char *zv_avatar_state_name(zv_avatar_state_t state)
{
    if ((unsigned)state >= (unsigned)ZV_AVATAR_STATE_COUNT) {
        return "unknown";
    }
    return k_avatar_state_names[state];
}
