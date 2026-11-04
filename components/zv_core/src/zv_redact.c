/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 */
#include "zv_redact.h"

#include <stddef.h>
#include <stdbool.h>
#include <string.h>

typedef struct {
    char *buf;
    size_t size;
    size_t len;
    bool truncated;
} redact_out_t;

static const char *const k_sensitive_keys[] = {
    "enrollment_code", "refresh_token", "access_token", "authorization", "private_key",
    "credential",      "signature",     "password",     "api_key",       "apikey",
    "passwd",          "secret",        "token",        "psk",           "pop",
};

static void emit(redact_out_t *out, const char *text, size_t n)
{
    for (size_t i = 0u; i < n; i++) {
        if (out->len + 1u >= out->size) {
            out->truncated = true;
            return;
        }
        out->buf[out->len++] = text[i];
    }
}

static void emit_marker(redact_out_t *out)
{
    emit(out, ZV_REDACTED_MARKER, sizeof(ZV_REDACTED_MARKER) - 1u);
}

static char to_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool is_alnum(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

static bool is_word(char c)
{
    return is_alnum(c) || c == '_';
}

static bool is_token_char(char c)
{
    return is_alnum(c) || c == '-' || c == '_' || c == '.' || c == '+' || c == '/' || c == '=';
}

static bool starts_with_ci(const char *text, const char *word)
{
    for (size_t i = 0u; word[i] != '\0'; i++) {
        if (text[i] == '\0' || to_lower(text[i]) != word[i]) {
            return false;
        }
    }
    return true;
}

static size_t skip_blanks(const char *text, size_t p)
{
    while (text[p] == ' ' || text[p] == '\t') {
        p++;
    }
    return p;
}

/* "Bearer <token>" -> "Bearer [REDACTED]". Returns consumed length or 0. */
static size_t try_bearer(const char *in, size_t i, redact_out_t *out)
{
    if (!starts_with_ci(&in[i], "bearer") || in[i + 6u] != ' ') {
        return 0u;
    }
    size_t p = skip_blanks(in, i + 6u);
    size_t start = p;
    while (in[p] != '\0' && strchr(" \t\r\n\"',;", in[p]) == NULL) {
        p++;
    }
    if (p == start) {
        return 0u;
    }
    emit(out, &in[i], start - i);
    emit_marker(out);
    return p - i;
}

/* key=value, key: value, "key": "value". Returns consumed length or 0. */
static size_t try_key_value(const char *in, size_t i, redact_out_t *out)
{
    for (size_t k = 0u; k < sizeof(k_sensitive_keys) / sizeof(k_sensitive_keys[0]); k++) {
        const char *key = k_sensitive_keys[k];
        size_t key_len = strlen(key);
        if (!starts_with_ci(&in[i], key) || is_word(in[i + key_len])) {
            continue;
        }
        size_t p = i + key_len;
        if (in[p] == '"' || in[p] == '\'') {
            p++;
        }
        p = skip_blanks(in, p);
        if (in[p] != ':' && in[p] != '=') {
            continue;
        }
        p = skip_blanks(in, p + 1u);
        char quote = '\0';
        if (in[p] == '"' || in[p] == '\'') {
            quote = in[p];
            p++;
        }
        size_t value_start = p;
        if (quote != '\0') {
            while (in[p] != '\0' && in[p] != quote) {
                p++;
            }
        } else if (strcmp(key, "authorization") == 0) {
            /* Header form: the whole remainder of the line is the credential. */
            while (in[p] != '\0' && in[p] != '\r' && in[p] != '\n') {
                p++;
            }
        } else {
            while (in[p] != '\0' && strchr(" \t\r\n,;&}]\"'", in[p]) == NULL) {
                p++;
            }
        }
        if (p == value_start) {
            return 0u;
        }
        emit(out, &in[i], value_start - i);
        emit_marker(out);
        return p - i;
    }
    return 0u;
}

static bool run_is_sensitive(const char *run, size_t len)
{
    if (len >= 20u && run[0] == 'e' && run[1] == 'y' && run[2] == 'J' && memchr(run, '.', len) != NULL) {
        return true;
    }
    if (len >= 20u && run[0] == 's' && run[1] == 'k' && run[2] == '-') {
        return true;
    }
    if (len < 40u) {
        return false;
    }
    bool upper = false;
    bool lower = false;
    bool digit = false;
    for (size_t i = 0u; i < len; i++) {
        upper = upper || (run[i] >= 'A' && run[i] <= 'Z');
        lower = lower || (run[i] >= 'a' && run[i] <= 'z');
        digit = digit || (run[i] >= '0' && run[i] <= '9');
    }
    return upper && lower && digit;
}

zv_status_t zv_redact(const char *input, char *output, size_t output_size, size_t *redactions)
{
    if (redactions != NULL) {
        *redactions = 0u;
    }
    if (input == NULL || output == NULL || output_size == 0u) {
        return ZV_ERR_INVALID_ARG;
    }

    redact_out_t out = {output, output_size, 0u, false};
    size_t count = 0u;
    size_t i = 0u;
    size_t run_end = 0u;

    while (input[i] != '\0') {
        if (is_word(input[i]) && (i == 0u || !is_word(input[i - 1u]))) {
            size_t consumed = try_bearer(input, i, &out);
            if (consumed == 0u) {
                consumed = try_key_value(input, i, &out);
            }
            if (consumed != 0u) {
                count++;
                i += consumed;
                run_end = i;
                continue;
            }
        }
        if (i >= run_end && is_token_char(input[i])) {
            size_t end = i;
            while (is_token_char(input[end])) {
                end++;
            }
            if (run_is_sensitive(&input[i], end - i)) {
                emit_marker(&out);
                count++;
                i = end;
                run_end = end;
                continue;
            }
            /* Keys may still start inside this run (e.g. "x-api_key:"), so keep scanning per char. */
            run_end = end;
        }
        emit(&out, &input[i], 1u);
        i++;
    }

    output[out.len] = '\0';
    if (redactions != NULL) {
        *redactions = count;
    }
    return out.truncated ? ZV_ERR_BUFFER_TOO_SMALL : ZV_OK;
}
