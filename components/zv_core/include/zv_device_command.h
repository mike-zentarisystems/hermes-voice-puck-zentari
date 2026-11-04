/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 *
 * Portable policy for gateway-issued device commands and the local device state
 * they act on. No ESP-IDF, no allocation, no I/O: the caller owns the socket,
 * the clock, and the effect implementation.
 *
 * Trust tiers are a property of the command name, fixed in the allowlist below.
 * The sender's `mutates` hint is parsed for diagnostics only and is never used
 * to decide enforcement.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "zv_approval.h"
#include "zv_status.h"
#include "zv_volume.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZV_CMD_NAME_MAX 47u        /* matches ZV_APPROVAL_ACTION_MAX */
#define ZV_CMD_ARGS_MAX 128u       /* bounded nested args object */
#define ZV_CMD_FRAME_MAX 1024u     /* bounded wire frame */
#define ZV_CMD_RESULT_MAX 256u     /* bounded device.command.result frame */
#define ZV_CMD_CANONICAL_MAX 256u
#define ZV_CMD_RISK "mutating"     /* the only risk a device command may carry */

/**
 * Transport requirement per tier.
 *   READ/PUSH  - authenticated session + allowlisted name only.
 *   MUTATING   - signed, single-use, exact-digest approval plus a hold gesture.
 */
typedef enum {
    ZV_CMD_TIER_NONE = 0,
    ZV_CMD_TIER_READ,
    ZV_CMD_TIER_PUSH,
    ZV_CMD_TIER_MUTATING,
} zv_device_command_tier_t;

typedef enum {
    ZV_CMD_STATUS_APPLIED = 0,
    ZV_CMD_STATUS_REJECTED,
    ZV_CMD_STATUS_FAILED,
    ZV_CMD_STATUS_EXPIRED,
} zv_device_status_t;

/** Bounded, wire-visible outcome. Never carries argument or summary content. */
typedef enum {
    ZV_CMD_REASON_NONE = 0,
    ZV_CMD_REASON_APPLIED,
    ZV_CMD_REASON_UNKNOWN_COMMAND,
    ZV_CMD_REASON_MALFORMED,
    ZV_CMD_REASON_UNAUTHORIZED,
    ZV_CMD_REASON_BUSY,
    ZV_CMD_REASON_LINK_UNAVAILABLE,
    ZV_CMD_REASON_SIGNATURE,
    ZV_CMD_REASON_REPLAY,
    ZV_CMD_REASON_EXPIRED,
    ZV_CMD_REASON_POLICY,
    ZV_CMD_REASON_UNSUPPORTED,
    ZV_CMD_REASON_BUFFER_TOO_SMALL,
} zv_device_reason_t;

/** Parsed device.command. The approval half is valid only when has_approval. */
typedef struct {
    char command_id[ZV_APPROVAL_ID_LEN + 1u];
    char command[ZV_CMD_NAME_MAX + 1u];
    char args[ZV_CMD_ARGS_MAX + 1u]; /**< raw bounded object body, "" when absent */
    bool mutates;                    /**< sender claim; diagnostics only */
    bool has_approval;
    zv_approval_request_t approval;
    char signature[ZV_APPROVAL_SIG_MAX + 1u];
    size_t signature_len;
} zv_device_command_t;

/**
 * Applies one already-gated command that has no portable model behind it. Must
 * not perform transport work and must not touch the display; it runs on
 * whichever task owns device state.
 */
typedef zv_status_t (*zv_device_cmd_apply_fn)(const char *command, const char *args, size_t args_len, void *ctx);

/**
 * Runtime context. Owns no globals.
 *
 * Level and mute are NOT modelled here: zv_volume is the single owner of that
 * state, so the UI shell and the command surface cannot disagree about what the
 * volume is. Muting remembers the operator's level rather than discarding it,
 * so `device.volume.set` while muted succeeds and takes effect on unmute.
 */
/** Trusted SHA-256 adapter over independently constructed command bytes. */
typedef bool (*zv_device_cmd_digest_verify_fn)(const char *canonical, size_t canonical_len,
                                               const char *expected_hex, void *ctx);

typedef struct {
    zv_volume_t volume;
    zv_device_cmd_apply_fn apply; /**< may be NULL; policy is still evaluated */
    void *apply_ctx;
    zv_device_cmd_digest_verify_fn digest_verify;
    void *digest_ctx;
    zv_device_command_t pending_command; /**< immutable verified snapshot */
    zv_hold_confirm_t hold;
    zv_approval_policy_t pending_policy;
    uint32_t last_hold_ms;
    bool hold_started;
    bool hold_pressed;
    bool session_online;           /**< caller-supplied link state */
    bool pending_mutating;         /**< one held mutating command at a time */
} zv_device_command_ctx_t;

void zv_device_command_init(zv_device_command_ctx_t *ctx);

/** Allowlist lookup. Returns ZV_CMD_TIER_NONE for an unknown name. */
zv_device_command_tier_t zv_device_command_tier(const char *command);
bool zv_device_command_name_valid(const char *command);

const char *zv_device_command_tier_name(zv_device_command_tier_t tier);
const char *zv_device_status_name(zv_device_status_t status);
const char *zv_device_reason_name(zv_device_reason_t reason);
zv_device_reason_t zv_device_reason_from_status(zv_status_t status);

/**
 * Strict bounded parse of one device.command frame. Rejects unknown top-level
 * keys, oversized args, a non-UUID command_id, and a half-present approval
 * block. Does not verify any signature.
 */
zv_status_t zv_device_command_parse(const char *frame, size_t frame_len, zv_device_command_t *out);

/** Sorted compact JSON: action, normalized args, command_id. No secret material. */
zv_status_t zv_device_command_canonical(const zv_device_command_t *cmd, char *buf, size_t size, size_t *len);

/**
 * Feed physical hold input and poll completion for the retained command.
 * First call starts arming, ignoring carry-over presses. ZV_OK with len=0 means
 * still waiting; a terminal decision emits one result and clears the snapshot.
 * Rechecks session and wall-clock expiry before applying. denial wins over hold.
 */
zv_status_t zv_device_command_confirm(zv_device_command_ctx_t *ctx, bool pressed, bool denied,
                                      uint32_t now_ms, int64_t now_s, char *result, size_t result_size,
                                      size_t *result_len, zv_device_reason_t *reason_out);

/**
 * Full fail-closed gate, in a fixed order: name allowlist, link availability,
 * busy check, approval presence consistency, tier/risk agreement, then
 * zv_approval_verify() for mutating commands. Non-mutating commands carrying
 * approval fields are rejected rather than ignored.
 *
 * On success *tier_out carries the allowlist-derived tier. A successful
 * mutating check retains the verified snapshot for zv_device_command_confirm().
 * hold_clear() cancels pending work; it never authorizes an effect. On failure *reason_out carries the wire-visible reason.
 */
zv_status_t zv_device_command_check(zv_device_command_ctx_t *ctx, const zv_device_command_t *cmd,
                                    const zv_approval_policy_t *policy, const zv_approval_gate_t *gate, int64_t now_s,
                                    zv_device_command_tier_t *tier_out, zv_device_reason_t *reason_out);

void zv_device_command_hold_clear(zv_device_command_ctx_t *ctx);

/**
 * Applies a command that has already passed zv_device_command_check().
 * MUTATING commands are refused here; use confirm() to complete their hold.
 * READ/PUSH effects route to
 * ctx->apply. Returns ZV_OK when the effect took hold.
 */
zv_status_t zv_device_command_apply(zv_device_command_ctx_t *ctx, const zv_device_command_t *cmd);

/**
 * Builds device.command.result. Never echoes args, summary, or digest.
 * Returns ZV_ERR_BUFFER_TOO_SMALL when the frame does not fit.
 */
zv_status_t zv_device_command_result_build(char *buf, size_t buf_size, const char *command_id,
                                           zv_device_status_t status, zv_device_reason_t reason);

/**
 * Ingress seam: one received control frame in, one device.command.result out.
 *
 * There is no control-frame dispatch layer in firmware yet, so this is where a
 * frame belongs the moment one exists: type-check, parse, gate, apply, then
 * build the reply. The caller owns the socket and the reply transport, and must
 * treat a non-ZV_OK return as "no reply was produced" rather than guessing a
 * command_id.
 *
 * A verified mutation is retained, never applied here: ZV_OK with result_len=0
 * means pending physical confirmation. confirm() produces its terminal result.
 * Otherwise ZV_OK means a result was written (including rejection). *result_len
 * excludes the terminating NUL. *reason_out always carries the outcome.
 */
zv_status_t zv_device_command_ingest(zv_device_command_ctx_t *ctx, const char *frame, size_t frame_len,
                                     const zv_approval_policy_t *policy, const zv_approval_gate_t *gate, int64_t now_s,
                                     char *result, size_t result_size, size_t *result_len,
                                     zv_device_reason_t *reason_out);

/**
 * Local operator path for the same effect names, with no approval hold.
 *
 * This asymmetry is deliberate: the hold constrains a remote or unauthenticated
 * actor. A finger on the local panel is the operator's own presence and is the
 * credential. Do not "fix" this by adding a hold - see docs/SECURITY.md.
 */
zv_status_t zv_device_command_apply_local(zv_device_command_ctx_t *ctx, const char *command, const char *args,
                                          size_t args_len);

/** Bounded integer/bool lookup inside a raw args object. */
bool zv_device_args_level(const char *args, size_t args_len, uint8_t *level_out);
bool zv_device_args_flag(const char *args, size_t args_len, bool *flag_out);

#ifdef __cplusplus
}
#endif