/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 *
 * Bounded, depth-aware parsing for device.command plus the tier gate.
 * Allocation-free and hardware-free by construction: every buffer is caller
 * owned and every length is bounded before use.
 */
#include "zv_device_command.h"

#include <stdio.h>
#include <string.h>

#include "zv_gateway.h"

#define ZV_JSON_MAX_KEYS 20u
#define ZV_JSON_KEY_MAX 32u
#define ZV_CMD_RISK_LEN 8u /* strlen("mutating") */

static const struct {
    const char *name;
    zv_device_command_tier_t tier;
} k_commands[] = {
    {"device.volume.get", ZV_CMD_TIER_READ},
    {"device.volume.set", ZV_CMD_TIER_MUTATING},
    {"device.mute.set", ZV_CMD_TIER_MUTATING},
    {"device.wake.trigger", ZV_CMD_TIER_PUSH},
    {"device.dismiss.press", ZV_CMD_TIER_MUTATING},
    {"device.menu.open", ZV_CMD_TIER_PUSH},
    {"device.menu.select", ZV_CMD_TIER_MUTATING},
};

static const char *const k_frame_keys[] = {
    "v",          "type",     "command_id", "command",  "args",          "mutates",
    "approval_id", "action",   "risk",       "summary",  "action_digest", "nonce",
    "kid",        "issued_at", "expires_at", "signature",
};

static size_t bounded_len(const char *s, size_t max)
{
    if (s == NULL) {
        return 0u;
    }
    size_t n = 0u;
    while (n < max && s[n] != '\0') {
        n++;
    }
    return n;
}

static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
        p++;
    }
    return p;
}

/*
 * Locates the value text for a top-level (depth-1) key in key position.
 * Depth awareness is what makes this safe: a value such as
 * "summary":"set command_id now" cannot be mistaken for the command_id field.
 * Returns NULL when the key is absent.
 */
static const char *find_top_value(const char *json, const char *name)
{
    if (json == NULL || name == NULL) {
        return NULL;
    }
    size_t name_len = strlen(name);
    size_t i = 0u;
    size_t depth = 0u;
    bool in_string = false;
    bool escaped = false;
    bool after_colon = false;

    while (json[i] != '\0') {
        char c = json[i];
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
            i++;
            continue;
        }
        if (c == '"') {
            if (depth == 1u && !after_colon) {
                size_t j = i + 1u;
                size_t key_len = 0u;
                bool key_escaped = false;
                while (json[j] != '\0' && json[j] != '"') {
                    if (key_escaped) {
                        key_escaped = false;
                    } else if (json[j] == '\\') {
                        key_escaped = true;
                    }
                    j++;
                    key_len++;
                }
                if (json[j] == '"' && key_len == name_len && memcmp(&json[i + 1u], name, name_len) == 0) {
                    const char *p = skip_ws(&json[j + 1u]);
                    if (*p != ':') {
                        return NULL;
                    }
                    return skip_ws(p + 1);
                }
            }
            in_string = true;
            i++;
            continue;
        }
        if (c == '{') {
            depth++;
        } else if (c == '}') {
            if (depth > 0u) {
                depth--;
            }
        } else if (c == ':') {
            after_colon = true;
        } else if (c == ',') {
            after_colon = false;
        }
        i++;
    }
    return NULL;
}

static bool json_string(const char *json, const char *name, char *out, size_t out_size)
{
    const char *start = find_top_value(json, name);
    if (start == NULL || *start != '"' || out == NULL || out_size == 0u) {
        return false;
    }
    start++;
    size_t n = 0u;
    while (start[n] != '\0' && start[n] != '"') {
        unsigned char c = (unsigned char)start[n];
        if (c == '\\' || c < 0x20u) {
            return false;
        }
        n++;
    }
    if (start[n] != '"' || n == 0u || n >= out_size) {
        return false;
    }
    memcpy(out, start, n);
    out[n] = '\0';
    return true;
}

static bool json_int(const char *json, const char *name, int64_t *out)
{
    const char *p = find_top_value(json, name);
    if (p == NULL || out == NULL) {
        return false;
    }
    bool negative = false;
    if (*p == '-') {
        negative = true;
        p++;
    }
    if (*p < '0' || *p > '9') {
        return false;
    }
    int64_t value = 0;
    size_t digits = 0u;
    while (*p >= '0' && *p <= '9') {
        if (value > (INT64_MAX - 9) / 10) {
            return false;
        }
        value = value * 10 + (*p - '0');
        p++;
        digits++;
    }
    if (digits == 0u || digits > 18u) {
        return false;
    }
    /* A trailing fractional part means this was not an integer field. */
    if (*p == '.' || *p == 'e' || *p == 'E') {
        return false;
    }
    *out = negative ? -value : value;
    return true;
}

static bool json_bool(const char *json, const char *name, bool *out)
{
    const char *p = find_top_value(json, name);
    if (p == NULL || out == NULL) {
        return false;
    }
    if (strncmp(p, "true", 4u) == 0) {
        *out = true;
        return true;
    }
    if (strncmp(p, "false", 5u) == 0) {
        *out = false;
        return true;
    }
    return false;
}

/* Copies a nested {...} object body, brace-matched with string awareness. */
static bool json_object(const char *json, const char *name, char *out, size_t out_size)
{
    const char *start = find_top_value(json, name);
    if (start == NULL || *start != '{' || out == NULL || out_size == 0u) {
        return false;
    }
    size_t i = 0u;
    size_t depth = 0u;
    bool in_string = false;
    bool escaped = false;
    while (start[i] != '\0') {
        char c = start[i];
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
            i++;
            continue;
        }
        if (c == '"') {
            in_string = true;
        } else if (c == '{') {
            depth++;
        } else if (c == '}') {
            depth--;
            if (depth == 0u) {
                i++;
                break;
            }
        }
        i++;
    }
    if (depth != 0u || i > ZV_CMD_ARGS_MAX || i >= out_size) {
        return false;
    }
    memcpy(out, start, i);
    out[i] = '\0';
    return true;
}

static bool top_keys_allowed(const char *json, const char *const *allowed, size_t allowed_count)
{
    const char *starts[ZV_JSON_MAX_KEYS];
    size_t lengths[ZV_JSON_MAX_KEYS];
    size_t count = 0u;
    size_t i = 0u;
    size_t depth = 0u;
    bool in_string = false;
    bool escaped = false;
    bool after_colon = false;
    size_t pending = 0u;
    bool pending_key = false;

    while (json[i] != '\0') {
        char c = json[i];
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
                if (pending_key) {
                    starts[count] = &json[pending];
                    lengths[count] = i - pending;
                    if (lengths[count] == 0u || lengths[count] >= ZV_JSON_KEY_MAX) {
                        return false;
                    }
                    count++;
                    pending_key = false;
                }
            }
            i++;
            continue;
        }
        if (c == '"') {
            if (depth == 1u && !after_colon) {
                if (count >= ZV_JSON_MAX_KEYS) {
                    return false;
                }
                pending = i + 1u;
                pending_key = true;
            }
            in_string = true;
            i++;
            continue;
        }
        if (c == '{') {
            depth++;
        } else if (c == '}') {
            if (depth > 0u) {
                depth--;
            }
        } else if (c == ':') {
            after_colon = true;
        } else if (c == ',') {
            after_colon = false;
        }
        i++;
    }
    if (in_string || pending_key) {
        return false;
    }
    for (size_t k = 0u; k < count; k++) {
        for (size_t previous = 0u; previous < k; previous++) {
            if (lengths[previous] == lengths[k] &&
                memcmp(starts[previous], starts[k], lengths[k]) == 0) return false;
        }
        bool matched = false;
        for (size_t a = 0u; a < allowed_count; a++) {
            size_t len = strlen(allowed[a]);
            if (len == lengths[k] && memcmp(starts[k], allowed[a], len) == 0) {
                matched = true;
                break;
            }
        }
        if (!matched) {
            return false;
        }
    }
    return true;
}

void zv_device_command_init(zv_device_command_ctx_t *ctx)
{
    if (ctx == NULL) {
        return;
    }
    memset(ctx, 0, sizeof(*ctx));
    zv_volume_init(&ctx->volume, 40u);
}

zv_device_command_tier_t zv_device_command_tier(const char *command)
{
    if (command == NULL) {
        return ZV_CMD_TIER_NONE;
    }
    for (size_t i = 0u; i < sizeof(k_commands) / sizeof(k_commands[0]); i++) {
        if (strcmp(command, k_commands[i].name) == 0) {
            return k_commands[i].tier;
        }
    }
    return ZV_CMD_TIER_NONE;
}

bool zv_device_command_name_valid(const char *command)
{
    size_t n = bounded_len(command, ZV_CMD_NAME_MAX + 1u);
    if (n == 0u || n > ZV_CMD_NAME_MAX) {
        return false;
    }
    for (size_t i = 0u; i < n; i++) {
        char c = command[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

const char *zv_device_command_tier_name(zv_device_command_tier_t tier)
{
    switch (tier) {
    case ZV_CMD_TIER_READ:
        return "read";
    case ZV_CMD_TIER_PUSH:
        return "push";
    case ZV_CMD_TIER_MUTATING:
        return "mutating";
    case ZV_CMD_TIER_NONE:
    default:
        return "none";
    }
}

const char *zv_device_status_name(zv_device_status_t status)
{
    switch (status) {
    case ZV_CMD_STATUS_APPLIED:
        return "applied";
    case ZV_CMD_STATUS_REJECTED:
        return "rejected";
    case ZV_CMD_STATUS_FAILED:
        return "failed";
    case ZV_CMD_STATUS_EXPIRED:
        return "expired";
    default:
        return "rejected";
    }
}

const char *zv_device_reason_name(zv_device_reason_t reason)
{
    switch (reason) {
    case ZV_CMD_REASON_NONE:
        return "none";
    case ZV_CMD_REASON_APPLIED:
        return "applied";
    case ZV_CMD_REASON_UNKNOWN_COMMAND:
        return "unknown_command";
    case ZV_CMD_REASON_MALFORMED:
        return "malformed";
    case ZV_CMD_REASON_UNAUTHORIZED:
        return "unauthorized";
    case ZV_CMD_REASON_BUSY:
        return "busy";
    case ZV_CMD_REASON_LINK_UNAVAILABLE:
        return "link_unavailable";
    case ZV_CMD_REASON_SIGNATURE:
        return "signature";
    case ZV_CMD_REASON_REPLAY:
        return "replay";
    case ZV_CMD_REASON_EXPIRED:
        return "expired";
    case ZV_CMD_REASON_POLICY:
        return "policy";
    case ZV_CMD_REASON_UNSUPPORTED:
        return "unsupported";
    case ZV_CMD_REASON_BUFFER_TOO_SMALL:
        return "buffer_too_small";
    default:
        return "malformed";
    }
}

zv_device_reason_t zv_device_reason_from_status(zv_status_t status)
{
    switch (status) {
    case ZV_OK:
        return ZV_CMD_REASON_NONE;
    case ZV_ERR_EXPIRED:
    case ZV_ERR_NOT_YET_VALID:
        return ZV_CMD_REASON_EXPIRED;
    case ZV_ERR_REPLAY:
        return ZV_CMD_REASON_REPLAY;
    case ZV_ERR_SIGNATURE:
        return ZV_CMD_REASON_SIGNATURE;
    case ZV_ERR_POLICY:
        return ZV_CMD_REASON_POLICY;
    case ZV_ERR_UNSUPPORTED:
        return ZV_CMD_REASON_UNSUPPORTED;
    case ZV_ERR_BUFFER_TOO_SMALL:
        return ZV_CMD_REASON_BUFFER_TOO_SMALL;
    case ZV_ERR_INVALID_ARG:
    default:
        return ZV_CMD_REASON_MALFORMED;
    }
}

/* Per-command argument schema. An unknown key is a malformed command, not a
 * silently ignored extra, so a sender cannot smuggle a second meaning past a
 * handler that only reads the key it knows about. */
static bool args_keys_allowed(const char *args, size_t args_len, const char *const *allowed, size_t allowed_count)
{
    if (args == NULL || args_len == 0u || args_len > ZV_CMD_ARGS_MAX || allowed_count == 0u) {
        return false;
    }
    char body[ZV_CMD_ARGS_MAX + 1u];
    memcpy(body, args, args_len);
    body[args_len] = '\0';
    return top_keys_allowed(body, allowed, allowed_count);
}

/* Exactly one unescaped schema key and one complete primitive token. */
static bool single_arg(const char *args, size_t args_len, const char *key, char *body, const char **value)
{
    if (args == NULL || args_len == 0u || args_len > ZV_CMD_ARGS_MAX ||
        memchr(args, '\0', args_len) != NULL) return false;
    memcpy(body, args, args_len);
    body[args_len] = '\0';
    const char *p = skip_ws(body);
    if (*p++ != '{') return false;
    p = skip_ws(p);
    if (*p++ != '"') return false;
    size_t key_len = strlen(key);
    if (strncmp(p, key, key_len) != 0) return false;
    p += key_len;
    if (*p++ != '"') return false;
    p = skip_ws(p);
    if (*p++ != ':') return false;
    *value = skip_ws(p);
    return true;
}

static bool arg_end(const char *p)
{
    p = skip_ws(p);
    if (*p != '}') return false;
    return *skip_ws(p + 1) == '\0';
}

bool zv_device_args_level(const char *args, size_t args_len, uint8_t *level_out)
{
    char body[ZV_CMD_ARGS_MAX + 1u];
    const char *p = NULL;
    if (level_out == NULL || !single_arg(args, args_len, "level", body, &p) ||
        *p < '0' || *p > '9') return false;
    bool leading_zero = *p == '0';
    unsigned int value = 0u;
    size_t digits = 0u;
    while (*p >= '0' && *p <= '9') {
        value = value * 10u + (unsigned int)(*p++ - '0');
        if (value > 100u || ++digits > 3u) return false;
    }
    if ((leading_zero && digits > 1u) || !arg_end(p)) return false;
    *level_out = (uint8_t)value;
    return true;
}

bool zv_device_args_flag(const char *args, size_t args_len, bool *flag_out)
{
    char body[ZV_CMD_ARGS_MAX + 1u];
    const char *p = NULL;
    if (flag_out == NULL || !single_arg(args, args_len, "muted", body, &p)) return false;
    if (strncmp(p, "true", 4u) == 0 && arg_end(p + 4)) { *flag_out = true; return true; }
    if (strncmp(p, "false", 5u) == 0 && arg_end(p + 5)) { *flag_out = false; return true; }
    return false;
}

static bool empty_args(const char *args, size_t len)
{
    if (len == 0u) return true;
    if (args == NULL || len > ZV_CMD_ARGS_MAX || memchr(args, '\0', len) != NULL) return false;
    char body[ZV_CMD_ARGS_MAX + 1u];
    memcpy(body, args, len);
    body[len] = '\0';
    const char *p = skip_ws(body);
    return *p == '{' && arg_end(p + 1);
}

zv_status_t zv_device_command_canonical(const zv_device_command_t *cmd, char *buf, size_t size, size_t *len)
{
    if (cmd == NULL || buf == NULL || size == 0u || len == NULL ||
        !zv_hello_id_is_uuid(cmd->command_id) || !zv_device_command_name_valid(cmd->command)) return ZV_ERR_INVALID_ARG;
    size_t args_len = bounded_len(cmd->args, sizeof(cmd->args));
    char normalized[32] = "{}";
    if (strcmp(cmd->command, "device.volume.set") == 0) {
        uint8_t level;
        if (!zv_device_args_level(cmd->args, args_len, &level)) return ZV_ERR_POLICY;
        (void)snprintf(normalized, sizeof(normalized), "{\"level\":%u}", (unsigned)level);
    } else if (strcmp(cmd->command, "device.mute.set") == 0) {
        bool muted;
        if (!zv_device_args_flag(cmd->args, args_len, &muted)) return ZV_ERR_POLICY;
        (void)snprintf(normalized, sizeof(normalized), "{\"muted\":%s}", muted ? "true" : "false");
    } else if (!empty_args(cmd->args, args_len)) {
        return ZV_ERR_POLICY;
    }
    int n = snprintf(buf, size, "{\"action\":\"%s\",\"args\":%s,\"command_id\":\"%s\"}",
                     cmd->command, normalized, cmd->command_id);
    if (n < 0 || (size_t)n >= size) return ZV_ERR_BUFFER_TOO_SMALL;
    *len = (size_t)n;
    return ZV_OK;
}

zv_status_t zv_device_command_parse(const char *frame, size_t frame_len, zv_device_command_t *out)
{
    if (frame == NULL || out == NULL) {
        return ZV_ERR_INVALID_ARG;
    }
    if (frame_len == 0u || frame_len > ZV_CMD_FRAME_MAX || bounded_len(frame, frame_len) != frame_len) {
        return ZV_ERR_INVALID_ARG;
    }
    /* The transport owns exactly frame_len bytes, not necessarily a terminator. */
    char json[ZV_CMD_FRAME_MAX + 1u];
    memcpy(json, frame, frame_len);
    json[frame_len] = '\0';
    frame = json;
    if (*skip_ws(frame) != '{') return ZV_ERR_INVALID_ARG;
    size_t tail = frame_len;
    while (tail > 0u && (frame[tail - 1u] == ' ' || frame[tail - 1u] == '\t' ||
                        frame[tail - 1u] == '\r' || frame[tail - 1u] == '\n')) tail--;
    if (tail == 0u || frame[tail - 1u] != '}') return ZV_ERR_INVALID_ARG;
    if (!top_keys_allowed(frame, k_frame_keys, sizeof(k_frame_keys) / sizeof(k_frame_keys[0]))) {
        return ZV_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));

    int64_t version = 0;
    if (!json_int(frame, "v", &version) || version != 1) {
        return ZV_ERR_INVALID_ARG;
    }
    char type[32];
    if (!json_string(frame, "type", type, sizeof(type)) || strcmp(type, "device.command") != 0) {
        return ZV_ERR_INVALID_ARG;
    }
    if (!json_string(frame, "command_id", out->command_id, sizeof(out->command_id)) ||
        !zv_hello_id_is_uuid(out->command_id)) {
        return ZV_ERR_INVALID_ARG;
    }
    if (!json_string(frame, "command", out->command, sizeof(out->command)) ||
        !zv_device_command_name_valid(out->command)) {
        return ZV_ERR_INVALID_ARG;
    }

    /* `args` is optional; `mutates` is a sender hint kept for diagnostics only. */
    if (find_top_value(frame, "args") != NULL) {
        if (!json_object(frame, "args", out->args, sizeof(out->args))) {
            return ZV_ERR_BUFFER_TOO_SMALL;
        }
    }
    if (find_top_value(frame, "mutates") != NULL && !json_bool(frame, "mutates", &out->mutates)) {
        return ZV_ERR_INVALID_ARG;
    }

    /* Approval fields are all-or-nothing: a half-present block is malformed. */
    static const char *const k_approval_names[] = {"approval_id", "action",   "risk", "action_digest",
                                                   "nonce",       "kid",      "issued_at", "expires_at",
                                                   "summary",     "signature"};
    size_t present = 0u;
    for (size_t i = 0u; i < sizeof(k_approval_names) / sizeof(k_approval_names[0]); i++) {
        if (find_top_value(frame, k_approval_names[i]) != NULL) {
            present++;
        }
    }
    if (present == 0u) {
        return ZV_OK;
    }
    if (present != sizeof(k_approval_names) / sizeof(k_approval_names[0])) {
        return ZV_ERR_INVALID_ARG;
    }

    out->has_approval = true;
    if (!json_string(frame, "approval_id", out->approval.approval_id, sizeof(out->approval.approval_id)) ||
        !json_string(frame, "action", out->approval.action, sizeof(out->approval.action)) ||
        !json_string(frame, "risk", out->approval.risk, sizeof(out->approval.risk)) ||
        !json_string(frame, "summary", out->approval.summary, sizeof(out->approval.summary)) ||
        !json_string(frame, "action_digest", out->approval.action_digest, sizeof(out->approval.action_digest)) ||
        !json_string(frame, "nonce", out->approval.nonce, sizeof(out->approval.nonce)) ||
        !json_string(frame, "kid", out->approval.kid, sizeof(out->approval.kid))) {
        return ZV_ERR_INVALID_ARG;
    }
    int64_t issued = 0;
    int64_t expires = 0;
    if (!json_int(frame, "issued_at", &issued) || !json_int(frame, "expires_at", &expires)) {
        return ZV_ERR_INVALID_ARG;
    }
    out->approval.issued_at_s = issued;
    out->approval.expires_at_s = expires;

    if (!json_string(frame, "signature", out->signature, sizeof(out->signature))) {
        return ZV_ERR_INVALID_ARG;
    }
    out->signature_len = bounded_len(out->signature, ZV_APPROVAL_SIG_MAX + 1u);
    if (out->signature_len < ZV_APPROVAL_SIG_MIN || out->signature_len > ZV_APPROVAL_SIG_MAX) {
        return ZV_ERR_INVALID_ARG;
    }
    return ZV_OK;
}

void zv_device_command_hold_clear(zv_device_command_ctx_t *ctx)
{
    if (ctx != NULL) {
        ctx->pending_mutating = false;
        ctx->hold_started = false;
        ctx->hold_pressed = false;
        memset(&ctx->pending_command, 0, sizeof(ctx->pending_command));
        memset(&ctx->hold, 0, sizeof(ctx->hold));
    }
}

zv_status_t zv_device_command_check(zv_device_command_ctx_t *ctx, const zv_device_command_t *cmd,
                                    const zv_approval_policy_t *policy, const zv_approval_gate_t *gate, int64_t now_s,
                                    zv_device_command_tier_t *tier_out, zv_device_reason_t *reason_out)
{
    zv_device_reason_t local_reason = ZV_CMD_REASON_MALFORMED;
    zv_device_command_tier_t tier = ZV_CMD_TIER_NONE;

    if (tier_out != NULL) {
        *tier_out = ZV_CMD_TIER_NONE;
    }
    if (reason_out != NULL) {
        *reason_out = ZV_CMD_REASON_MALFORMED;
    }
    if (ctx == NULL || cmd == NULL || policy == NULL) {
        return ZV_ERR_INVALID_ARG;
    }

    /* 1. Name allowlist. Never dynamic-dispatch on an unrecognised name. */
    tier = zv_device_command_tier(cmd->command);
    if (tier == ZV_CMD_TIER_NONE) {
        local_reason = ZV_CMD_REASON_UNKNOWN_COMMAND;
        goto reject;
    }

    /* 2. The link must be usable; a degraded session cannot mutate the device. */
    if (!ctx->session_online) {
        local_reason = ZV_CMD_REASON_LINK_UNAVAILABLE;
        goto reject;
    }

    /* 3. One held mutating command at a time. Queueing would let a stale
     *    command fire minutes later, after the operator's attention moved on. */
    if (tier == ZV_CMD_TIER_MUTATING && ctx->pending_mutating) {
        local_reason = ZV_CMD_REASON_BUSY;
        goto reject;
    }

    /* 4. Approval presence must agree with the tier in both directions. */
    if (tier == ZV_CMD_TIER_MUTATING) {
        if (!cmd->has_approval) {
            local_reason = ZV_CMD_REASON_UNAUTHORIZED;
            goto reject;
        }
    } else if (cmd->has_approval) {
        local_reason = ZV_CMD_REASON_MALFORMED;
        goto reject;
    }

    if (tier == ZV_CMD_TIER_MUTATING) {
        /* 5. A device command may only ever claim the mutating tier. The
         *    destructive tier belongs to Hermes-originated work and must not be
         *    reachable through this surface. */
        if (bounded_len(cmd->approval.risk, sizeof(cmd->approval.risk)) != ZV_CMD_RISK_LEN ||
            memcmp(cmd->approval.risk, ZV_CMD_RISK, ZV_CMD_RISK_LEN) != 0) {
            local_reason = ZV_CMD_REASON_UNAUTHORIZED;
            goto reject;
        }
        if (gate == NULL || gate->trust == NULL || gate->nonce_cache == NULL || gate->verify == NULL) {
            local_reason = ZV_CMD_REASON_UNAUTHORIZED;
            goto reject;
        }
        char canonical[ZV_CMD_CANONICAL_MAX];
        size_t canonical_len = 0u;
        if (strcmp(cmd->approval.action, cmd->command) != 0 || ctx->digest_verify == NULL ||
            zv_device_command_canonical(cmd, canonical, sizeof(canonical), &canonical_len) != ZV_OK ||
            !ctx->digest_verify(canonical, canonical_len, cmd->approval.action_digest, ctx->digest_ctx)) {
            local_reason = ZV_CMD_REASON_UNAUTHORIZED;
            goto reject;
        }
        /* 6. Full ordered gate: signature format, structure, time, key, then
         *    signature verification, with the nonce reserved last. */
        zv_status_t status = zv_approval_verify(&cmd->approval, cmd->signature, cmd->signature_len, now_s, policy, gate);
        if (status != ZV_OK) {
            local_reason = zv_device_reason_from_status(status);
            goto reject;
        }
        ctx->pending_command = *cmd;
        ctx->pending_policy = *policy;
        ctx->hold_started = false;
        ctx->pending_mutating = true;
    }

    if (tier_out != NULL) {
        *tier_out = tier;
    }
    if (reason_out != NULL) {
        *reason_out = ZV_CMD_REASON_NONE;
    }
    return ZV_OK;

reject:
    if (reason_out != NULL) {
        *reason_out = local_reason;
    }
    return ZV_ERR_POLICY;
}

static zv_status_t apply_one(zv_device_command_ctx_t *ctx, const char *command, const char *args, size_t args_len)
{
    if (strcmp(command, "device.volume.set") == 0) {
        static const char *const k_level_only[] = {"level"};
        uint8_t level = 0u;
        /* Strict at the protocol edge: an out-of-range arg is a malformed
         * command, not a value to clamp. zv_volume clamps defensively for the
         * other caller (the UI slider drag), which is a legitimate source of
         * out-of-range coordinates. Two callers, two different contracts. */
        if (!args_keys_allowed(args, args_len, k_level_only, 1u) || !zv_device_args_level(args, args_len, &level)) {
            return ZV_ERR_POLICY;
        }
        return zv_volume_set_level(&ctx->volume, level);
    }
    if (strcmp(command, "device.mute.set") == 0) {
        static const char *const k_muted_only[] = {"muted"};
        bool muted = false;
        if (!args_keys_allowed(args, args_len, k_muted_only, 1u) || !zv_device_args_flag(args, args_len, &muted)) {
            return ZV_ERR_POLICY;
        }
        return zv_volume_set_muted(&ctx->volume, muted);
    }
    if (strcmp(command, "device.volume.get") == 0) {
        /* Accepted and applied; the value stays readable through zv_volume.
         * device.command.result deliberately carries no payload, so returning a
         * level over the wire needs its own read frame. */
        return ZV_OK;
    }
    if (!empty_args(args, args_len)) return ZV_ERR_POLICY;
    if (ctx->apply == NULL) {
        return ZV_ERR_UNSUPPORTED;
    }
    return ctx->apply(command, args, args_len, ctx->apply_ctx);
}

zv_status_t zv_device_command_apply(zv_device_command_ctx_t *ctx, const zv_device_command_t *cmd)
{
    if (ctx == NULL || cmd == NULL) {
        return ZV_ERR_INVALID_ARG;
    }
    /* Mutations must execute the retained snapshot through confirm(), never a
     * caller's mutable copy or a generic 'already gated' effect call. */
    zv_device_command_tier_t tier = zv_device_command_tier(cmd->command);
    if (!ctx->session_online || tier == ZV_CMD_TIER_NONE || tier == ZV_CMD_TIER_MUTATING) return ZV_ERR_POLICY;
    return apply_one(ctx, cmd->command, cmd->args, bounded_len(cmd->args, sizeof(cmd->args)));
}

zv_status_t zv_device_command_apply_local(zv_device_command_ctx_t *ctx, const char *command, const char *args,
                                          size_t args_len)
{
    if (ctx == NULL || command == NULL) {
        return ZV_ERR_INVALID_ARG;
    }
    /* Same allowlist as the wire path: local input gets no wider a vocabulary
     * than a signed command does. Only the approval hold is skipped. */
    if (zv_device_command_tier(command) == ZV_CMD_TIER_NONE) {
        return ZV_ERR_POLICY;
    }
    return apply_one(ctx, command, args, args_len);
}

zv_status_t zv_device_command_result_build(char *buf, size_t buf_size, const char *command_id,
                                           zv_device_status_t status, zv_device_reason_t reason)
{
    if (buf == NULL || buf_size == 0u || command_id == NULL || !zv_hello_id_is_uuid(command_id)) {
        return ZV_ERR_INVALID_ARG;
    }
    const char *status_text = zv_device_status_name(status);
    const char *reason_text = zv_device_reason_name(reason);
    int n = snprintf(buf, buf_size,
                     "{\"v\":1,\"type\":\"device.command.result\",\"command_id\":\"%s\","
                     "\"status\":\"%s\",\"reason_code\":\"%s\"}",
                     command_id, status_text, reason_text);
    if (n <= 0 || (size_t)n >= buf_size) {
        return ZV_ERR_BUFFER_TOO_SMALL;
    }
    return ZV_OK;
}

/* Recover only an unambiguous, top-level UUID from a bounded command frame.
 * This path never authorizes or applies the rejected payload. */
static bool command_id_recoverable(const char *frame, size_t frame_len, char *out, size_t out_size)
{
    if (frame_len == 0u || frame_len > ZV_CMD_FRAME_MAX ||
        bounded_len(frame, frame_len) != frame_len) return false;
    char json[ZV_CMD_FRAME_MAX + 1u];
    memcpy(json, frame, frame_len);
    json[frame_len] = '\0';
    if (*skip_ws(json) != '{') return false;
    size_t depth = 0u, ids = 0u;
    bool key_position = false;
    for (size_t i = 0u; i < frame_len; i++) {
        char c = json[i];
        if (c == '"') {
            size_t begin = ++i;
            bool escaped = false, has_escape = false;
            for (; i < frame_len; i++) {
                c = json[i];
                if (escaped) { escaped = false; continue; }
                if (c == '\\') { escaped = true; has_escape = true; continue; }
                if (c == '"') break;
            }
            if (i == frame_len) return false;
            if (depth == 1u && key_position && !has_escape && i - begin == 10u &&
                memcmp(json + begin, "command_id", 10u) == 0) {
                if (++ids != 1u) return false;
                const char *value = skip_ws(json + i + 1u);
                if (*value != ':') return false;
                value = skip_ws(value + 1);
                if (*value != '"') return false;
                value++;
                size_t remaining = frame_len - (size_t)(value - json);
                if (remaining < 37u || value[36] != '"' || out_size < 37u) return false;
                memcpy(out, value, 36u);
                out[36] = '\0';
                if (!zv_hello_id_is_uuid(out)) return false;
                char delimiter = *skip_ws(value + 37u);
                if (delimiter != ',' && delimiter != '}') return false;
            }
        } else if (c == '{' || c == '[') {
            depth++;
            if (depth == 1u) key_position = true;
        } else if (c == '}' || c == ']') {
            if (depth == 0u) return false;
            depth--;
            if (depth == 0u && *skip_ws(json + i + 1u) != '\0') return false;
        } else if (depth == 1u && c == ':') {
            key_position = false;
        } else if (depth == 1u && c == ',') {
            key_position = true;
        }
    }
    return depth == 0u && ids == 1u;
}

zv_status_t zv_device_command_ingest(zv_device_command_ctx_t *ctx, const char *frame, size_t frame_len,
                                     const zv_approval_policy_t *policy, const zv_approval_gate_t *gate, int64_t now_s,
                                     char *result, size_t result_size, size_t *result_len,
                                     zv_device_reason_t *reason_out)
{
    zv_device_reason_t reason = ZV_CMD_REASON_MALFORMED;
    if (reason_out != NULL) {
        *reason_out = ZV_CMD_REASON_MALFORMED;
    }
    if (result_len != NULL) {
        *result_len = 0u;
    }
    if (ctx == NULL || frame == NULL || result == NULL || result_size == 0u) {
        return ZV_ERR_INVALID_ARG;
    }

    zv_device_command_t cmd;
    zv_status_t status = zv_device_command_parse(frame, frame_len, &cmd);
    if (status != ZV_OK) {
        /* A frame too malformed to yield a command_id cannot be answered: the
         * result schema requires a UUID and inventing one would let a sender
         * correlate a reply to a command it never sent. */
        if (!command_id_recoverable(frame, frame_len, cmd.command_id, sizeof(cmd.command_id))) {
            return ZV_ERR_INVALID_ARG;
        }
        reason = zv_device_reason_from_status(status);
        status = zv_device_command_result_build(result, result_size, cmd.command_id, ZV_CMD_STATUS_REJECTED, reason);
        if (status != ZV_OK) {
            return status;
        }
        if (reason_out != NULL) {
            *reason_out = reason;
        }
        if (result_len != NULL) {
            *result_len = strlen(result);
        }
        return ZV_OK;
    }

    zv_device_command_tier_t tier = ZV_CMD_TIER_NONE;
    status = zv_device_command_check(ctx, &cmd, policy, gate, now_s, &tier, &reason);
    if (status == ZV_OK && tier == ZV_CMD_TIER_MUTATING) {
        result[0] = '\0';
        if (reason_out != NULL) *reason_out = ZV_CMD_REASON_NONE;
        return ZV_OK; /* Pending local hold, not an execution acknowledgement. */
    }
    if (status == ZV_OK) {
        status = zv_device_command_apply(ctx, &cmd);
        reason = status == ZV_OK ? ZV_CMD_REASON_APPLIED : zv_device_reason_from_status(status);
        /* READ/PUSH exchanges must not cancel a different pending mutation. */
    }

    zv_device_status_t reply_status = ZV_CMD_STATUS_APPLIED;
    if (reason != ZV_CMD_REASON_APPLIED) {
        reply_status = (reason == ZV_CMD_REASON_EXPIRED) ? ZV_CMD_STATUS_EXPIRED : ZV_CMD_STATUS_REJECTED;
    }
    status = zv_device_command_result_build(result, result_size, cmd.command_id, reply_status, reason);
    if (status != ZV_OK) {
        return status;
    }
    if (reason_out != NULL) {
        *reason_out = reason;
    }
    if (result_len != NULL) {
        *result_len = strlen(result);
    }
    return ZV_OK;
}
/* No external CONFIRMED enum is accepted: physical input drives our own hold. */
zv_status_t zv_device_command_confirm(zv_device_command_ctx_t *ctx, bool pressed, bool denied,
                                      uint32_t now_ms, int64_t now_s, char *result, size_t result_size,
                                      size_t *result_len, zv_device_reason_t *reason_out)
{
    if (result_len != NULL) *result_len = 0u;
    if (reason_out != NULL) *reason_out = ZV_CMD_REASON_NONE;
    if (ctx == NULL || result == NULL || result_size == 0u || !ctx->pending_mutating) return ZV_ERR_INVALID_ARG;
    result[0] = '\0';
    zv_device_reason_t reason = ZV_CMD_REASON_NONE;
    if (!ctx->session_online) reason = ZV_CMD_REASON_LINK_UNAVAILABLE;
    else if (now_s >= ctx->pending_command.approval.expires_at_s) reason = ZV_CMD_REASON_EXPIRED;
    else if (zv_approval_validate(&ctx->pending_command.approval, now_s, &ctx->pending_policy) != ZV_OK)
        reason = ZV_CMD_REASON_UNAUTHORIZED;
    else if (denied) reason = ZV_CMD_REASON_UNAUTHORIZED;
    else {
        if (!ctx->hold_started) {
            int64_t remaining = ctx->pending_command.approval.expires_at_s - now_s;
            uint32_t ttl = remaining > 60 ? 60000u : (uint32_t)remaining * 1000u;
            zv_status_t init = zv_hold_init(&ctx->hold, now_ms, ZV_HOLD_DEFAULT_ARM_DELAY_MS,
                                            ZV_HOLD_DEFAULT_REQUIRED_MS, ttl);
            if (init != ZV_OK) return init;
            ctx->hold_started = true;
            ctx->hold_pressed = pressed;
            ctx->last_hold_ms = now_ms;
            return ZV_OK; /* Ignore any press carried over from the previous screen. */
        }
        if ((uint32_t)(now_ms - ctx->last_hold_ms) > (uint32_t)INT32_MAX) {
            zv_device_command_hold_clear(ctx);
            if (reason_out != NULL) *reason_out = ZV_CMD_REASON_POLICY;
            return ZV_ERR_POLICY;
        }
        ctx->last_hold_ms = now_ms;
        zv_hold_state_t state;
        if (!pressed) state = zv_hold_release(&ctx->hold, now_ms);
        else if (!ctx->hold_pressed) state = zv_hold_press(&ctx->hold, now_ms);
        else state = zv_hold_tick(&ctx->hold, now_ms);
        ctx->hold_pressed = pressed; /* Only a fresh edge may start the hold. */
        if (state == ZV_HOLD_EXPIRED) reason = ZV_CMD_REASON_EXPIRED;
        else if (state != ZV_HOLD_CONFIRMED) return ZV_OK;
        else {
            zv_status_t applied = apply_one(ctx, ctx->pending_command.command, ctx->pending_command.args,
                                           bounded_len(ctx->pending_command.args, sizeof(ctx->pending_command.args)));
            reason = applied == ZV_OK ? ZV_CMD_REASON_APPLIED : zv_device_reason_from_status(applied);
        }
    }
    zv_device_status_t reply = reason == ZV_CMD_REASON_APPLIED ? ZV_CMD_STATUS_APPLIED :
                              reason == ZV_CMD_REASON_EXPIRED ? ZV_CMD_STATUS_EXPIRED : ZV_CMD_STATUS_REJECTED;
    zv_status_t built = zv_device_command_result_build(result, result_size, ctx->pending_command.command_id, reply, reason);
    zv_device_command_hold_clear(ctx); /* Includes failure and an undersized result buffer; never execute twice. */
    if (reason_out != NULL) *reason_out = reason;
    if (built == ZV_OK && result_len != NULL) *result_len = strlen(result);
    return built;
}
