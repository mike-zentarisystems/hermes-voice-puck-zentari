/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 */
#include "zv_gateway.h"

#include <stdio.h>
#include <string.h>

#include "zv_identity.h"

static bool is_hex(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool zv_hello_id_is_uuid(const char *text)
{
    if (text == NULL) {
        return false;
    }
    static const int groups[5] = {8, 4, 4, 4, 12};
    size_t i = 0;
    for (int g = 0; g < 5; g++) {
        if (g > 0) {
            if (text[i] != '-') {
                return false;
            }
            i++;
        }
        for (int n = 0; n < groups[g]; n++) {
            if (!is_hex(text[i])) {
                return false;
            }
            i++;
        }
    }
    return text[i] == '\0';
}

static bool device_id_ok(const char *id)
{
    size_t n = strlen(id);
    if (n < 8u || n > 64u) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) {
            return false;
        }
    }
    return true;
}

static bool firmware_ok(const char *fw)
{
    int idx = 0;
    const char *p = fw;
    while (idx < 3) {
        if (*p < '0' || *p > '9') {
            return false;
        }
        while (*p >= '0' && *p <= '9') {
            p++;
        }
        idx++;
        if (idx == 3) {
            break;
        }
        if (*p != '.') {
            return false;
        }
        p++;
    }
    return *p == '\0';
}

zv_status_t zv_gateway_parse_origin(const char *origin, zv_gateway_endpoint_t *out)
{
    if (origin == NULL || out == NULL) {
        return ZV_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    if (strncmp(origin, "wss://", 6) != 0) {
        return ZV_ERR_POLICY;
    }
    const char *p = origin + 6;
    if (*p == '\0' || *p == '@' || *p == '/' || *p == '?' || *p == '#') {
        return ZV_ERR_POLICY;
    }
    if (strchr(p, '@') != NULL || strchr(p, '#') != NULL || strchr(p, '?') != NULL) {
        return ZV_ERR_POLICY;
    }

    const char *slash = strchr(p, '/');
    const char *host_end = (slash != NULL) ? slash : p + strlen(p);
    if (host_end == p) {
        return ZV_ERR_POLICY;
    }

    const char *colon = NULL;
    for (const char *c = p; c < host_end; c++) {
        if (*c == ':') {
            colon = c;
        }
    }

    size_t host_len = (colon != NULL) ? (size_t)(colon - p) : (size_t)(host_end - p);
    if (host_len == 0u || host_len >= sizeof(out->host)) {
        return ZV_ERR_POLICY;
    }
    memcpy(out->host, p, host_len);
    out->host[host_len] = '\0';

    out->port = 443u;
    if (colon != NULL) {
        unsigned port = 0;
        const char *digits = colon + 1;
        if (digits >= host_end) {
            return ZV_ERR_POLICY;
        }
        for (const char *c = digits; c < host_end; c++) {
            if (*c < '0' || *c > '9') {
                return ZV_ERR_POLICY;
            }
            port = port * 10u + (unsigned)(*c - '0');
            if (port > 65535u) {
                return ZV_ERR_POLICY;
            }
        }
        if (port == 0u) {
            return ZV_ERR_POLICY;
        }
        out->port = (uint16_t)port;
    }

    if (slash != NULL) {
        size_t path_len = strlen(slash);
        if (path_len >= sizeof(out->path)) {
            return ZV_ERR_POLICY;
        }
        memcpy(out->path, slash, path_len + 1u);
    } else {
        out->path[0] = '/';
        out->path[1] = '\0';
    }
    out->use_tls = true;
    return ZV_OK;
}

static bool host_equal_ascii_nocase(const char *a, const char *b)
{
    for (; *a != '\0' && *b != '\0'; a++, b++) {
        char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a - 'A' + 'a') : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b - 'A' + 'a') : *b;
        if (ca != cb) {
            return false;
        }
    }
    return *a == *b;
}

bool zv_gateway_origin_matches(const char *a, const char *b)
{
    zv_gateway_endpoint_t ea;
    zv_gateway_endpoint_t eb;
    if (zv_gateway_parse_origin(a, &ea) != ZV_OK || zv_gateway_parse_origin(b, &eb) != ZV_OK) {
        return false;
    }
    return ea.port == eb.port && host_equal_ascii_nocase(ea.host, eb.host) && strcmp(ea.path, eb.path) == 0;
}

zv_status_t zv_hello_build(char *buf, size_t buf_size, const char *message_id, const char *device_id,
                           const char *firmware)
{
    if (buf == NULL || message_id == NULL || device_id == NULL || firmware == NULL || buf_size == 0u) {
        return ZV_ERR_INVALID_ARG;
    }
    if (!zv_hello_id_is_uuid(message_id) || !device_id_ok(device_id) || !firmware_ok(firmware)) {
        return ZV_ERR_POLICY;
    }
    int n = snprintf(buf, buf_size,
                     "{\"v\":1,\"type\":\"device.hello\",\"id\":\"%s\",\"device_id\":\"%s\","
                     "\"board_id\":\"%s\",\"firmware\":\"%s\"}",
                     message_id, device_id, ZV_BOARD_ID, firmware);
    if (n <= 0 || (size_t)n >= buf_size) {
        return ZV_ERR_BUFFER_TOO_SMALL;
    }
    return ZV_OK;
}
