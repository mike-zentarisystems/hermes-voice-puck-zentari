/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stddef.h>

#include "zv_status.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Replacement marker written in place of sensitive values. */
#define ZV_REDACTED_MARKER "[REDACTED]"

/**
 * Copies `input` to `output` with secrets replaced by ZV_REDACTED_MARKER.
 * Redacted:
 *  - `Bearer <token>` values;
 *  - values of sensitive keys (token, access_token, refresh_token, secret,
 *    password, passwd, psk, api_key, apikey, authorization, credential,
 *    enrollment_code, pop, private_key, signature) in `key=value`,
 *    `key: value` and JSON `"key": "value"` forms (case-insensitive);
 *  - JWT-shaped strings (`eyJ...` with a dot, >= 20 chars);
 *  - provider-style keys starting with `sk-` (>= 20 chars);
 *  - opaque tokens >= 40 chars mixing upper case, lower case and digits.
 * Pure lower-case hex (SHA-256 digests, MAC addresses) is preserved.
 *
 * Output is always NUL-terminated when output_size > 0. Returns
 * ZV_ERR_BUFFER_TOO_SMALL if truncated; *redactions (optional) receives the
 * number of replaced values.
 */
zv_status_t zv_redact(const char *input, char *output, size_t output_size, size_t *redactions);

#ifdef __cplusplus
}
#endif
