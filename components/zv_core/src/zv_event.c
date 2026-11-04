/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stddef.h>

#include "zv_event.h"

bool zv_event_is_critical(zv_event_type_t type)
{
    return !(type == ZV_EVT_AUDIO_LEVEL || type == ZV_EVT_VISEME);
}

static const char *const k_event_names[ZV_EVT_COUNT] = {
    [ZV_EVT_NONE] = "none",
    [ZV_EVT_BOOT_COMPLETE] = "boot_complete",
    [ZV_EVT_PROVISIONING_DONE] = "provisioning_done",
    [ZV_EVT_LINK_UP] = "link_up",
    [ZV_EVT_LINK_DOWN] = "link_down",
    [ZV_EVT_LINK_DEGRADED] = "link_degraded",
    [ZV_EVT_WAKE] = "wake",
    [ZV_EVT_LISTEN_END] = "listen_end",
    [ZV_EVT_RESPONSE_AUDIO_START] = "response_audio_start",
    [ZV_EVT_RESPONSE_AUDIO_END] = "response_audio_end",
    [ZV_EVT_ACTION_EXECUTING] = "action_executing",
    [ZV_EVT_ACTION_SUCCEEDED] = "action_succeeded",
    [ZV_EVT_ACTION_FAILED] = "action_failed",
    [ZV_EVT_APPROVAL_REQUESTED] = "approval_requested",
    [ZV_EVT_APPROVAL_RESOLVED] = "approval_resolved",
    [ZV_EVT_CANCEL] = "cancel",
    [ZV_EVT_ERROR] = "error",
    [ZV_EVT_TIMEOUT] = "timeout",
    [ZV_EVT_OTA_START] = "ota_start",
    [ZV_EVT_OTA_FAILED] = "ota_failed",
    [ZV_EVT_AUDIO_LEVEL] = "audio_level",
    [ZV_EVT_VISEME] = "viseme",
    [ZV_EVT_NOTIFICATION] = "notification",
};

const char *zv_event_type_name(zv_event_type_t type)
{
    if ((unsigned)type >= (unsigned)ZV_EVT_COUNT || k_event_names[type] == NULL) {
        return "unknown";
    }
    return k_event_names[type];
}
