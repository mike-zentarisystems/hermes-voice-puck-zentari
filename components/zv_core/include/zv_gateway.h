/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 *
 * Portable gateway transport policy and device.hello framing.
 * No sockets: the caller performs I/O after these checks pass.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "zv_status.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZV_HELLO_ID_MAX 64
#define ZV_HELLO_DEVICE_MAX 64
#define ZV_HELLO_FW_MAX 32
#define ZV_HELLO_JSON_MAX 256

/** One parsed gateway endpoint. Origin must be wss://host[:port][/path]. */
typedef struct {
    char host[96];
    char path[96];
    uint16_t port;
    bool use_tls; /**< always true when validation succeeds */
} zv_gateway_endpoint_t;

/**
 * Parses and policy-checks a gateway origin.
 * Requires scheme wss, a non-empty host, optional port (default 443), and
 * rejects credentials, fragments, and query strings in the origin.
 */
zv_status_t zv_gateway_parse_origin(const char *origin, zv_gateway_endpoint_t *out);

/**
 * True when both origins pass zv_gateway_parse_origin and name the same
 * endpoint: host (ASCII case-insensitive), port and path. A device credential
 * is enrolled for one origin; a build that dials a different one cannot
 * authenticate there, so callers should refuse to connect instead.
 */
bool zv_gateway_origin_matches(const char *a, const char *b);

/**
 * Builds a schema-shaped device.hello control frame.
 * `message_id` must be a UUID string (8-4-4-4-12 hex). `device_id` must match
 * ^[a-z0-9-]{8,64}$. `firmware` must be major.minor.patch.
 */
zv_status_t zv_hello_build(char *buf, size_t buf_size, const char *message_id, const char *device_id,
                           const char *firmware);

/** True when `text` looks like a UUID (hex groups 8-4-4-4-12, dashes only). */
bool zv_hello_id_is_uuid(const char *text);

#ifdef __cplusplus
}
#endif
