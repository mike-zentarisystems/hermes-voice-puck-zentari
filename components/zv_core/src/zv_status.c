/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 */
#include "zv_status.h"

const char *zv_status_str(zv_status_t status)
{
    switch (status) {
    case ZV_OK:
        return "ZV_OK";
    case ZV_ERR_INVALID_ARG:
        return "ZV_ERR_INVALID_ARG";
    case ZV_ERR_INVALID_STATE:
        return "ZV_ERR_INVALID_STATE";
    case ZV_ERR_INVALID_TRANSITION:
        return "ZV_ERR_INVALID_TRANSITION";
    case ZV_ERR_QUEUE_FULL:
        return "ZV_ERR_QUEUE_FULL";
    case ZV_ERR_QUEUE_EMPTY:
        return "ZV_ERR_QUEUE_EMPTY";
    case ZV_ERR_BUFFER_TOO_SMALL:
        return "ZV_ERR_BUFFER_TOO_SMALL";
    case ZV_ERR_EXPIRED:
        return "ZV_ERR_EXPIRED";
    case ZV_ERR_NOT_YET_VALID:
        return "ZV_ERR_NOT_YET_VALID";
    case ZV_ERR_REPLAY:
        return "ZV_ERR_REPLAY";
    case ZV_ERR_SIGNATURE:
        return "ZV_ERR_SIGNATURE";
    case ZV_ERR_POLICY:
        return "ZV_ERR_POLICY";
    case ZV_ERR_CIRCUIT_OPEN:
        return "ZV_ERR_CIRCUIT_OPEN";
    case ZV_ERR_UNSUPPORTED:
        return "ZV_ERR_UNSUPPORTED";
    case ZV_ERR_HARDWARE_GATE:
        return "ZV_ERR_HARDWARE_GATE";
    case ZV_ERR_COUNT:
        break;
    }
    return "ZV_ERR_UNKNOWN";
}
