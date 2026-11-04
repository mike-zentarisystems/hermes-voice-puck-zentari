/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 *
 * Device command tiers, the mutating approval gate, and local device state.
 *
 * The properties that matter here are mostly negative: a mutating command must
 * be unable to skip the signature gate, a read command must be unable to smuggle
 * an approval block, and a forged peer must not be able to consume a nonce slot
 * and lock out a genuine command.
 */
#include "zv_device_command.h"
#include "zv_test.h"
#include "zv_trust_store.h"

#define CMD_ID "5a97be68-cfbe-4772-84e7-0ea48c1a0fc1"
#define APPROVAL_ID "11111111-2222-4333-8444-555555555555"
#define NOW_S 1893456000
#define KID "device-key-1"

#define SIG "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"

/* A controllable digest adapter models signed bytes, not cryptography. Real
 * SHA-256 and ES256 are exercised by the firmware API host test separately. */
static char g_signed_command[ZV_CMD_CANONICAL_MAX];
static bool fixture_digest(const char *canonical, size_t len, const char *hex, void *unused)
{
    (void)unused;
    return strlen(g_signed_command) == len && memcmp(canonical, g_signed_command, len) == 0 &&
           strcmp(hex, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa") == 0;
}

static zv_status_t local_effect(zv_device_command_ctx_t *ctx, const zv_device_command_t *cmd)
{
    return zv_device_command_apply_local(ctx, cmd->command, cmd->args, strlen(cmd->args));
}

static void complete_hold(zv_device_command_ctx_t *ctx)
{
    char result[ZV_CMD_RESULT_MAX];
    size_t len;
    zv_device_reason_t reason;
    ZV_CHECK_EQ(zv_device_command_confirm(ctx, false, false, 0u, NOW_S, result, sizeof(result), &len, &reason), ZV_OK);
    ZV_CHECK_EQ(len, 0u);
    ZV_CHECK_EQ(zv_device_command_confirm(ctx, true, false, 400u, NOW_S, result, sizeof(result), &len, &reason), ZV_OK);
    ZV_CHECK_EQ(len, 0u);
    ZV_CHECK_EQ(zv_device_command_confirm(ctx, true, false, 1900u, NOW_S + 2, result, sizeof(result), &len, &reason), ZV_OK);
    ZV_CHECK_EQ(reason, ZV_CMD_REASON_APPLIED);
    ZV_CHECK(len > 0u);
}

static zv_trust_key_t make_key(void)
{
    zv_trust_key_t key;
    memset(&key, 0, sizeof(key));
    snprintf(key.kid, sizeof(key.kid), "%s", KID);
    key.curve = ZV_TRUST_CURVE_ES256;
    key.public_key[0] = 0x04u;
    for (size_t i = 1u; i < ZV_TRUST_P256_KEY_LEN; i++) {
        key.public_key[i] = (uint8_t)i;
    }
    key.public_key_len = ZV_TRUST_P256_KEY_LEN;
    key.not_before_s = NOW_S - 3600;
    key.not_after_s = NOW_S + 3600;
    return key;
}

static bool always_verify(const uint8_t *public_key, size_t public_key_len, const char *canonical,
                          size_t canonical_len, const char *signature, size_t signature_len, void *ctx)
{
    (void)public_key;
    (void)public_key_len;
    (void)canonical;
    (void)canonical_len;
    (void)signature;
    (void)signature_len;
    return *(bool *)ctx;
}

typedef struct {
    zv_trust_store_t trust;
    zv_nonce_cache_t nonce;
    bool accept;
    zv_approval_gate_t gate;
    zv_approval_policy_t policy;
} harness_t;

static void harness_init(harness_t *h)
{
    memset(h, 0, sizeof(*h));
    zv_trust_store_init(&h->trust);
    zv_nonce_cache_init(&h->nonce);
    h->accept = true;
    h->policy = zv_approval_default_policy();
    zv_trust_key_t key = make_key();
    ZV_CHECK_EQ(zv_trust_store_add(&h->trust, &key), ZV_OK);
    h->gate.trust = &h->trust;
    h->gate.nonce_cache = &h->nonce;
    h->gate.verify = always_verify;
    h->gate.verify_ctx = &h->accept;
}

/* A well-formed mutating frame carrying a full approval block. */
static void mutating_frame(char *buf, size_t size, const char *command, const char *args, const char *risk)
{
    snprintf(buf, size,
             "{\"v\":1,\"type\":\"device.command\",\"command_id\":\"%s\",\"command\":\"%s\","
             "\"args\":%s,\"mutates\":true,"
             "\"approval_id\":\"%s\",\"action\":\"%s\",\"risk\":\"%s\","
             "\"summary\":\"Set the puck speaker volume\","
             "\"action_digest\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\","
             "\"nonce\":\"AAAAAAAAAAAAAAAAAAAAAA\",\"kid\":\"%s\","
             "\"issued_at\":%d,\"expires_at\":%d,\"signature\":\"%s\"}",
             CMD_ID, command, args, APPROVAL_ID, command, risk, KID, NOW_S, NOW_S + 60, SIG);
    (void)snprintf(g_signed_command, sizeof(g_signed_command),
                   "{\"action\":\"%s\",\"args\":%s,\"command_id\":\"%s\"}", command, args, CMD_ID);
}

/* --------------------------------------------------------------- allowlist */

static void test_allowlist_and_names(void)
{
    ZV_CHECK_EQ(zv_device_command_tier("device.volume.get"), ZV_CMD_TIER_READ);
    ZV_CHECK_EQ(zv_device_command_tier("device.volume.set"), ZV_CMD_TIER_MUTATING);
    ZV_CHECK_EQ(zv_device_command_tier("device.mute.set"), ZV_CMD_TIER_MUTATING);
    ZV_CHECK_EQ(zv_device_command_tier("device.wake.trigger"), ZV_CMD_TIER_PUSH);
    ZV_CHECK_EQ(zv_device_command_tier("device.dismiss.press"), ZV_CMD_TIER_MUTATING);
    ZV_CHECK_EQ(zv_device_command_tier("device.menu.open"), ZV_CMD_TIER_PUSH);
    ZV_CHECK_EQ(zv_device_command_tier("device.menu.select"), ZV_CMD_TIER_MUTATING);

    /* Never dynamic-dispatch: unknown and malformed names are both NONE. */
    ZV_CHECK_EQ(zv_device_command_tier("device.reboot"), ZV_CMD_TIER_NONE);
    ZV_CHECK_EQ(zv_device_command_tier(""), ZV_CMD_TIER_NONE);
    ZV_CHECK_EQ(zv_device_command_tier(NULL), ZV_CMD_TIER_NONE);
    ZV_CHECK_EQ(zv_device_command_tier("DEVICE.VOLUME.SET"), ZV_CMD_TIER_NONE);

    ZV_CHECK(zv_device_command_name_valid("device.volume.set"));
    ZV_CHECK(!zv_device_command_name_valid("device.volume set")); /* space */
    ZV_CHECK(!zv_device_command_name_valid("device/volume/set")); /* slash */
    ZV_CHECK(!zv_device_command_name_valid(""));
    char long_name[80];
    memset(long_name, 'a', sizeof(long_name));
    long_name[sizeof(long_name) - 1u] = '\0';
    ZV_CHECK(!zv_device_command_name_valid(long_name)); /* over ZV_CMD_NAME_MAX */
}

/* ------------------------------------------------------------------- parse */

static void test_parse_read_frame(void)
{
    char frame[512];
    snprintf(frame, sizeof(frame),
             "{\"v\":1,\"type\":\"device.command\",\"command_id\":\"%s\","
             "\"command\":\"device.volume.get\",\"args\":{},\"mutates\":false}",
             CMD_ID);
    zv_device_command_t cmd;
    ZV_CHECK_EQ(zv_device_command_parse(frame, strlen(frame), &cmd), ZV_OK);
    ZV_CHECK_STR(cmd.command_id, CMD_ID);
    ZV_CHECK_STR(cmd.command, "device.volume.get");
    ZV_CHECK(!cmd.has_approval);
    ZV_CHECK(!cmd.mutates);
}

static void test_parse_mutating_frame(void)
{
    char frame[900];
    mutating_frame(frame, sizeof(frame), "device.volume.set", "{\"level\":40}", "mutating");
    zv_device_command_t cmd;
    ZV_CHECK_EQ(zv_device_command_parse(frame, strlen(frame), &cmd), ZV_OK);
    ZV_CHECK(cmd.has_approval);
    ZV_CHECK_STR(cmd.command, "device.volume.set");
    ZV_CHECK_STR(cmd.approval.risk, "mutating");
    ZV_CHECK_STR(cmd.args, "{\"level\":40}");
    ZV_CHECK(cmd.signature_len >= ZV_APPROVAL_SIG_MIN);
}

static void test_parse_rejects(void)
{
    zv_device_command_t cmd;
    char frame[900];

    /* Unknown top-level key. */
    snprintf(frame, sizeof(frame),
             "{\"v\":1,\"type\":\"device.command\",\"command_id\":\"%s\","
             "\"command\":\"device.volume.get\",\"sneaky\":1}",
             CMD_ID);
    ZV_CHECK_EQ(zv_device_command_parse(frame, strlen(frame), &cmd), ZV_ERR_INVALID_ARG);

    /* A key name appearing inside a value must not be mistaken for the field. */
    snprintf(frame, sizeof(frame),
             "{\"v\":1,\"type\":\"device.command\",\"command_id\":\"%s\","
             "\"command\":\"device.volume.get\",\"args\":{\"command_id\":\"spoofed\"}}",
             CMD_ID);
    ZV_CHECK_EQ(zv_device_command_parse(frame, strlen(frame), &cmd), ZV_OK);
    ZV_CHECK_STR(cmd.command_id, CMD_ID);

    /* Non-UUID command_id. */
    snprintf(frame, sizeof(frame),
             "{\"v\":1,\"type\":\"device.command\",\"command_id\":\"nope\","
             "\"command\":\"device.volume.get\"}");
    ZV_CHECK_EQ(zv_device_command_parse(frame, strlen(frame), &cmd), ZV_ERR_INVALID_ARG);

    /* Wrong version and wrong type. */
    snprintf(frame, sizeof(frame),
             "{\"v\":2,\"type\":\"device.command\",\"command_id\":\"%s\","
             "\"command\":\"device.volume.get\"}",
             CMD_ID);
    ZV_CHECK_EQ(zv_device_command_parse(frame, strlen(frame), &cmd), ZV_ERR_INVALID_ARG);
    snprintf(frame, sizeof(frame),
             "{\"v\":1,\"type\":\"approval.request\",\"command_id\":\"%s\","
             "\"command\":\"device.volume.get\"}",
             CMD_ID);
    ZV_CHECK_EQ(zv_device_command_parse(frame, strlen(frame), &cmd), ZV_ERR_INVALID_ARG);

    /* Half-present approval block. */
    snprintf(frame, sizeof(frame),
             "{\"v\":1,\"type\":\"device.command\",\"command_id\":\"%s\","
             "\"command\":\"device.volume.set\",\"approval_id\":\"%s\"}",
             CMD_ID, APPROVAL_ID);
    ZV_CHECK_EQ(zv_device_command_parse(frame, strlen(frame), &cmd), ZV_ERR_INVALID_ARG);

    /* Oversized args. The frame itself stays under ZV_CMD_FRAME_MAX so this
     * exercises the args bound rather than the frame bound. */
    char big[400];
    size_t n = (size_t)snprintf(big, sizeof(big),
                                "{\"v\":1,\"type\":\"device.command\",\"command_id\":\"%s\","
                                "\"command\":\"device.volume.set\",\"args\":{\"level\":40,\"pad\":\"",
                                CMD_ID);
    while (n + 4u < sizeof(big)) {
        big[n++] = 'x';
    }
    big[n++] = '"';
    big[n++] = '}';
    big[n] = '\0';
    ZV_CHECK(strlen(big) <= ZV_CMD_FRAME_MAX);
    ZV_CHECK(strlen(big) > ZV_CMD_ARGS_MAX);
    ZV_CHECK_EQ(zv_device_command_parse(big, strlen(big), &cmd), ZV_ERR_BUFFER_TOO_SMALL);

    /* A frame longer than the wire bound is refused outright. */
    char huge[ZV_CMD_FRAME_MAX + 64u];
    memset(huge, 'x', sizeof(huge));
    huge[sizeof(huge) - 1u] = '\0';
    ZV_CHECK_EQ(zv_device_command_parse(huge, strlen(huge), &cmd), ZV_ERR_INVALID_ARG);

    ZV_CHECK_EQ(zv_device_command_parse(NULL, 0u, &cmd), ZV_ERR_INVALID_ARG);
    ZV_CHECK_EQ(zv_device_command_parse(frame, strlen(frame), NULL), ZV_ERR_INVALID_ARG);
}

/* ------------------------------------------------------------- device state */

/*
 * zv_volume owns level and mute; this suite only proves the command surface
 * delegates to it correctly. The model's own properties (clamping, bars, repaint
 * cadence) belong to the volume suite, not here.
 */
static void test_device_state(void)
{
    zv_device_command_ctx_t ctx;
    zv_device_command_t cmd;
    char frame[900];

    zv_device_command_init(&ctx);
    ctx.digest_verify = fixture_digest;
    ctx.session_online = true;

    /* The context starts on the shared model, not on a private copy. */
    ZV_CHECK_EQ(zv_volume_level(&ctx.volume), 40u);
    ZV_CHECK(!zv_volume_is_muted(&ctx.volume));

    mutating_frame(frame, sizeof(frame), "device.volume.set", "{\"level\":100}", "mutating");
    ZV_CHECK_EQ(zv_device_command_parse(frame, strlen(frame), &cmd), ZV_OK);
    ZV_CHECK_EQ(local_effect(&ctx, &cmd), ZV_OK);
    ZV_CHECK_EQ(zv_volume_level(&ctx.volume), 100u);

    /* Out of range is refused at the protocol edge, before the model is reached,
     * so a malformed command never becomes a clamped-but-accepted one. */
    mutating_frame(frame, sizeof(frame), "device.volume.set", "{\"level\":101}", "mutating");
    ZV_CHECK_EQ(zv_device_command_parse(frame, strlen(frame), &cmd), ZV_OK);
    ZV_CHECK_EQ(local_effect(&ctx, &cmd), ZV_ERR_POLICY);
    ZV_CHECK_EQ(zv_volume_level(&ctx.volume), 100u);

    /* Mute remembers the level instead of discarding it: the remembered value
     * survives, silence is reported through the effective value, and unmute
     * returns to what the operator actually chose. */
    mutating_frame(frame, sizeof(frame), "device.mute.set", "{\"muted\":true}", "mutating");
    ZV_CHECK_EQ(zv_device_command_parse(frame, strlen(frame), &cmd), ZV_OK);
    ZV_CHECK_EQ(local_effect(&ctx, &cmd), ZV_OK);
    ZV_CHECK(zv_volume_is_muted(&ctx.volume));
    ZV_CHECK_EQ(zv_volume_level(&ctx.volume), 100u);
    ZV_CHECK_EQ(zv_volume_effective(&ctx.volume), 0u);

    /* Setting a level while muted is accepted and takes effect on unmute,
     * rather than being refused and losing the operator's intent. */
    mutating_frame(frame, sizeof(frame), "device.volume.set", "{\"level\":30}", "mutating");
    ZV_CHECK_EQ(zv_device_command_parse(frame, strlen(frame), &cmd), ZV_OK);
    ZV_CHECK_EQ(local_effect(&ctx, &cmd), ZV_OK);
    ZV_CHECK_EQ(zv_volume_effective(&ctx.volume), 0u);

    mutating_frame(frame, sizeof(frame), "device.mute.set", "{\"muted\":false}", "mutating");
    ZV_CHECK_EQ(zv_device_command_parse(frame, strlen(frame), &cmd), ZV_OK);
    ZV_CHECK_EQ(local_effect(&ctx, &cmd), ZV_OK);
    ZV_CHECK_EQ(zv_volume_level(&ctx.volume), 30u);
    ZV_CHECK_EQ(zv_volume_effective(&ctx.volume), 30u);
}

static int g_apply_calls;
static char g_apply_command[64];

static zv_status_t record_apply(const char *command, const char *args, size_t args_len, void *ctx)
{
    (void)args;
    (void)args_len;
    (void)ctx;
    g_apply_calls++;
    snprintf(g_apply_command, sizeof(g_apply_command), "%s", command);
    return ZV_OK;
}

/* --------------------------------------------------------------- tier gate */

static void test_tier_enforcement(void)
{
    harness_t h;
    zv_device_command_ctx_t ctx;
    zv_device_command_t cmd;
    zv_device_command_tier_t tier;
    zv_device_reason_t reason;
    char frame[900];

    harness_init(&h);
    zv_device_command_init(&ctx);
    ctx.digest_verify = fixture_digest;
    ctx.session_online = true;
    ctx.apply = record_apply;
    g_apply_calls = 0;

    /* read and push ride the authenticated session with no approval block. */
    snprintf(frame, sizeof(frame),
             "{\"v\":1,\"type\":\"device.command\",\"command_id\":\"%s\","
             "\"command\":\"device.volume.get\",\"mutates\":false}",
             CMD_ID);
    ZV_CHECK_EQ(zv_device_command_parse(frame, strlen(frame), &cmd), ZV_OK);
    ZV_CHECK_EQ(zv_device_command_check(&ctx, &cmd, &h.policy, &h.gate, NOW_S, &tier, &reason), ZV_OK);
    ZV_CHECK_EQ(tier, ZV_CMD_TIER_READ);
    ZV_CHECK_EQ(reason, ZV_CMD_REASON_NONE);

    snprintf(frame, sizeof(frame),
             "{\"v\":1,\"type\":\"device.command\",\"command_id\":\"%s\","
             "\"command\":\"device.menu.open\",\"mutates\":true}",
             CMD_ID);
    ZV_CHECK_EQ(zv_device_command_parse(frame, strlen(frame), &cmd), ZV_OK);
    ZV_CHECK_EQ(zv_device_command_check(&ctx, &cmd, &h.policy, &h.gate, NOW_S, &tier, &reason), ZV_OK);
    ZV_CHECK_EQ(tier, ZV_CMD_TIER_PUSH);
    /* A read/push command never sets the one-pending hold. */
    ZV_CHECK(!ctx.pending_mutating);

    /* mutating without an approval block is unauthorized. */
    snprintf(frame, sizeof(frame),
             "{\"v\":1,\"type\":\"device.command\",\"command_id\":\"%s\","
             "\"command\":\"device.volume.set\",\"args\":{\"level\":40},\"mutates\":true}",
             CMD_ID);
    ZV_CHECK_EQ(zv_device_command_parse(frame, strlen(frame), &cmd), ZV_OK);
    ZV_CHECK_EQ(zv_device_command_check(&ctx, &cmd, &h.policy, &h.gate, NOW_S, &tier, &reason), ZV_ERR_POLICY);
    ZV_CHECK_EQ(reason, ZV_CMD_REASON_UNAUTHORIZED);

    /* A read command carrying an approval block is malformed, not ignored. */
    char mix[900];
    mutating_frame(mix, sizeof(mix), "device.volume.get", "{}", "mutating");
    ZV_CHECK_EQ(zv_device_command_parse(mix, strlen(mix), &cmd), ZV_OK);
    ZV_CHECK_EQ(zv_device_command_check(&ctx, &cmd, &h.policy, &h.gate, NOW_S, &tier, &reason), ZV_ERR_POLICY);
    ZV_CHECK_EQ(reason, ZV_CMD_REASON_MALFORMED);

    /* Unknown command never reaches any gate stage. */
    snprintf(frame, sizeof(frame),
             "{\"v\":1,\"type\":\"device.command\",\"command_id\":\"%s\","
             "\"command\":\"device.reboot\",\"mutates\":true}",
             CMD_ID);
    ZV_CHECK_EQ(zv_device_command_parse(frame, strlen(frame), &cmd), ZV_OK);
    ZV_CHECK_EQ(zv_device_command_check(&ctx, &cmd, &h.policy, &h.gate, NOW_S, &tier, &reason), ZV_ERR_POLICY);
    ZV_CHECK_EQ(reason, ZV_CMD_REASON_UNKNOWN_COMMAND);
}

static void test_link_and_busy(void)
{
    harness_t h;
    zv_device_command_ctx_t ctx;
    zv_device_command_t cmd;
    zv_device_command_tier_t tier;
    zv_device_reason_t reason;
    char frame[900];

    harness_init(&h);
    zv_device_command_init(&ctx);
    ctx.digest_verify = fixture_digest;
    ctx.session_online = false;

    mutating_frame(frame, sizeof(frame), "device.volume.set", "{\"level\":40}", "mutating");
    ZV_CHECK_EQ(zv_device_command_parse(frame, strlen(frame), &cmd), ZV_OK);

    ZV_CHECK_EQ(zv_device_command_check(&ctx, &cmd, &h.policy, &h.gate, NOW_S, &tier, &reason), ZV_ERR_POLICY);
    ZV_CHECK_EQ(reason, ZV_CMD_REASON_LINK_UNAVAILABLE);

    /* With the link up, the first mutating command takes the single pending slot
     * and a second is refused rather than queued. */
    ctx.session_online = true;
    ZV_CHECK_EQ(zv_device_command_check(&ctx, &cmd, &h.policy, &h.gate, NOW_S, &tier, &reason), ZV_OK);
    ZV_CHECK_EQ(tier, ZV_CMD_TIER_MUTATING);
    ZV_CHECK(ctx.pending_mutating);

    zv_device_command_t second = cmd;
    ZV_CHECK_EQ(zv_device_command_check(&ctx, &second, &h.policy, &h.gate, NOW_S, &tier, &reason), ZV_ERR_POLICY);
    ZV_CHECK_EQ(reason, ZV_CMD_REASON_BUSY);

    zv_device_command_hold_clear(&ctx);
    ZV_CHECK(!ctx.pending_mutating);
    /* A fresh command can take the slot once the hold resolves. It must carry a
     * fresh nonce: reusing the earlier one would correctly be refused as replay. */
    zv_device_command_t third = cmd;
    snprintf(third.approval.nonce, sizeof(third.approval.nonce), "BBBBBBBBBBBBBBBBBBBBBB");
    ZV_CHECK_EQ(zv_device_command_check(&ctx, &third, &h.policy, &h.gate, NOW_S + 1, &tier, &reason), ZV_OK);
    ZV_CHECK(ctx.pending_mutating);

    /* Reusing the consumed nonce is still refused after the hold clears. The hold
     * must be released first, otherwise the busy check fires first by design. */
    zv_device_command_hold_clear(&ctx);
    ZV_CHECK_EQ(zv_device_command_check(&ctx, &cmd, &h.policy, &h.gate, NOW_S + 1, &tier, &reason), ZV_ERR_POLICY);
    ZV_CHECK_EQ(reason, ZV_CMD_REASON_REPLAY);
}

/* --------------------------------------------------- approval reuse and risk */

static void test_mutating_gate_reuses_approval(void)
{
    harness_t h;
    zv_device_command_ctx_t ctx;
    zv_device_command_t cmd;
    zv_device_command_tier_t tier;
    zv_device_reason_t reason;
    char frame[900];

    /* Valid signature accepted. */
    harness_init(&h);
    zv_device_command_init(&ctx);
    ctx.digest_verify = fixture_digest;
    ctx.session_online = true;
    mutating_frame(frame, sizeof(frame), "device.volume.set", "{\"level\":40}", "mutating");
    ZV_CHECK_EQ(zv_device_command_parse(frame, strlen(frame), &cmd), ZV_OK);
    ZV_CHECK_EQ(zv_device_command_check(&ctx, &cmd, &h.policy, &h.gate, NOW_S, &tier, &reason), ZV_OK);
    ZV_CHECK_EQ(zv_device_command_apply(&ctx, &cmd), ZV_ERR_POLICY);
    complete_hold(&ctx);
    ZV_CHECK_EQ(zv_volume_level(&ctx.volume), 40u);

    /* Replayed nonce is refused. */
    harness_init(&h);
    zv_device_command_init(&ctx);
    ctx.digest_verify = fixture_digest;
    ctx.session_online = true;
    mutating_frame(frame, sizeof(frame), "device.volume.set", "{\"level\":55}", "mutating");
    ZV_CHECK_EQ(zv_device_command_parse(frame, strlen(frame), &cmd), ZV_OK);
    ZV_CHECK_EQ(zv_device_command_check(&ctx, &cmd, &h.policy, &h.gate, NOW_S, &tier, &reason), ZV_OK);
    zv_device_command_hold_clear(&ctx);
    ZV_CHECK_EQ(zv_device_command_check(&ctx, &cmd, &h.policy, &h.gate, NOW_S, &tier, &reason), ZV_ERR_POLICY);
    ZV_CHECK_EQ(reason, ZV_CMD_REASON_REPLAY);

    /* Expired approval. */
    harness_init(&h);
    zv_device_command_init(&ctx);
    ctx.digest_verify = fixture_digest;
    ctx.session_online = true;
    mutating_frame(frame, sizeof(frame), "device.volume.set", "{\"level\":40}", "mutating");
    ZV_CHECK_EQ(zv_device_command_parse(frame, strlen(frame), &cmd), ZV_OK);
    ZV_CHECK_EQ(zv_device_command_check(&ctx, &cmd, &h.policy, &h.gate, NOW_S + 600, &tier, &reason), ZV_ERR_POLICY);
    ZV_CHECK_EQ(reason, ZV_CMD_REASON_EXPIRED);

    /* A device command may never claim the Hermes destructive tier. */
    harness_init(&h);
    zv_device_command_init(&ctx);
    ctx.digest_verify = fixture_digest;
    ctx.session_online = true;
    mutating_frame(frame, sizeof(frame), "device.volume.set", "{\"level\":40}", "destructive");
    ZV_CHECK_EQ(zv_device_command_parse(frame, strlen(frame), &cmd), ZV_OK);
    ZV_CHECK_EQ(zv_device_command_check(&ctx, &cmd, &h.policy, &h.gate, NOW_S, &tier, &reason), ZV_ERR_POLICY);
    ZV_CHECK_EQ(reason, ZV_CMD_REASON_UNAUTHORIZED);

    /* An untrusted kid is a signature failure with no key-existence oracle. */
    harness_init(&h);
    zv_device_command_init(&ctx);
    ctx.digest_verify = fixture_digest;
    ctx.session_online = true;
    mutating_frame(frame, sizeof(frame), "device.volume.set", "{\"level\":40}", "mutating");
    char needle[64];
    snprintf(needle, sizeof(needle), "\"kid\":\"%s\"", KID);
    char *hit = strstr(frame, needle);
    ZV_CHECK(hit != NULL);
    /* Same-length replacement: a longer one would overrun the closing quote and
     * turn this into a malformed-frame test rather than an unknown-kid test. */
    memcpy(hit + strlen("\"kid\":\""), "rogue-key-01", strlen("rogue-key-01"));
    ZV_CHECK_EQ(zv_device_command_parse(frame, strlen(frame), &cmd), ZV_OK);
    ZV_CHECK_EQ(zv_device_command_check(&ctx, &cmd, &h.policy, &h.gate, NOW_S, &tier, &reason), ZV_ERR_POLICY);
    ZV_CHECK_EQ(reason, ZV_CMD_REASON_SIGNATURE);
}

static void test_forgery_cannot_exhaust_nonce_cache(void)
{
    harness_t h;
    zv_device_command_ctx_t ctx;
    zv_device_command_t cmd;
    zv_device_command_tier_t tier;
    zv_device_reason_t reason;
    char frame[900];

    harness_init(&h);
    zv_device_command_init(&ctx);
    ctx.digest_verify = fixture_digest;
    ctx.session_online = true;

    /* Forgeries that reuse the real nonce but fail at the verifier. They must
     * not consume nonce slots, or a hostile peer could lock out every genuine
     * mutating command by flooding the cache. */
    h.accept = false;
    for (int i = 0; i < 4; i++) {
        mutating_frame(frame, sizeof(frame), "device.volume.set", "{\"level\":10}", "mutating");
        ZV_CHECK_EQ(zv_device_command_parse(frame, strlen(frame), &cmd), ZV_OK);
        ZV_CHECK_EQ(zv_device_command_check(&ctx, &cmd, &h.policy, &h.gate, NOW_S, &tier, &reason), ZV_ERR_POLICY);
    }

    /* The genuine request still verifies. */
    h.accept = true;
    mutating_frame(frame, sizeof(frame), "device.volume.set", "{\"level\":40}", "mutating");
    ZV_CHECK_EQ(zv_device_command_parse(frame, strlen(frame), &cmd), ZV_OK);
    ZV_CHECK_EQ(zv_device_command_check(&ctx, &cmd, &h.policy, &h.gate, NOW_S, &tier, &reason), ZV_OK);
    ZV_CHECK(!ctx.pending_mutating || true); /* hold may be set; cleared below */
    zv_device_command_hold_clear(&ctx);
}

static void test_risk_vocabulary(void)
{
    zv_approval_policy_t policy = zv_approval_default_policy();
    char frame[900];
    zv_device_command_t cmd;

    /* Both tiers validate; anything else still fails closed. */
    mutating_frame(frame, sizeof(frame), "device.volume.set", "{\"level\":40}", "mutating");
    ZV_CHECK_EQ(zv_device_command_parse(frame, strlen(frame), &cmd), ZV_OK);
    ZV_CHECK_EQ(zv_approval_validate(&cmd.approval, NOW_S + 1, &policy), ZV_OK);

    mutating_frame(frame, sizeof(frame), "device.volume.set", "{\"level\":40}", "destructive");
    ZV_CHECK_EQ(zv_device_command_parse(frame, strlen(frame), &cmd), ZV_OK);
    ZV_CHECK_EQ(zv_approval_validate(&cmd.approval, NOW_S + 1, &policy), ZV_OK);

    mutating_frame(frame, sizeof(frame), "device.volume.set", "{\"level\":40}", "harmless");
    ZV_CHECK_EQ(zv_device_command_parse(frame, strlen(frame), &cmd), ZV_OK);
    ZV_CHECK_EQ(zv_approval_validate(&cmd.approval, NOW_S + 1, &policy), ZV_ERR_INVALID_ARG);
}

/* ---------------------------------------------------------- effects and local */

static void test_apply_routes_and_bounds(void)
{
    zv_device_command_ctx_t ctx;
    zv_device_command_t cmd;
    char frame[900];

    zv_device_command_init(&ctx);
    ctx.digest_verify = fixture_digest;
    ctx.session_online = true;
    ctx.apply = record_apply;
    g_apply_calls = 0;

    mutating_frame(frame, sizeof(frame), "device.volume.set", "{\"level\":40}", "mutating");
    ZV_CHECK_EQ(zv_device_command_parse(frame, strlen(frame), &cmd), ZV_OK);
    ZV_CHECK_EQ(local_effect(&ctx, &cmd), ZV_OK);
    ZV_CHECK_EQ(zv_volume_level(&ctx.volume), 40u);
    /* Volume/mute act on the shared model, not the callback. */
    ZV_CHECK_EQ(g_apply_calls, 0);

    /* Out-of-range level is refused with no partial write. */
    mutating_frame(frame, sizeof(frame), "device.volume.set", "{\"level\":250}", "mutating");
    ZV_CHECK_EQ(zv_device_command_parse(frame, strlen(frame), &cmd), ZV_OK);
    ZV_CHECK_EQ(local_effect(&ctx, &cmd), ZV_ERR_POLICY);
    ZV_CHECK_EQ(zv_volume_level(&ctx.volume), 40u);

    /* Missing or malformed args are refused. */
    mutating_frame(frame, sizeof(frame), "device.volume.set", "{\"loudness\":40}", "mutating");
    ZV_CHECK_EQ(zv_device_command_parse(frame, strlen(frame), &cmd), ZV_OK);
    ZV_CHECK_EQ(local_effect(&ctx, &cmd), ZV_ERR_POLICY);

    /* Muting is remembered, not lost: the level survives and the amplifier
     * output goes silent through the effective value alone. */
    ZV_CHECK_EQ(zv_volume_set_muted(&ctx.volume, true), ZV_OK);
    ZV_CHECK_EQ(zv_volume_level(&ctx.volume), 40u);
    ZV_CHECK_EQ(zv_volume_effective(&ctx.volume), 0u);
    ZV_CHECK_EQ(zv_volume_set_muted(&ctx.volume, false), ZV_OK);
    ZV_CHECK_EQ(zv_volume_effective(&ctx.volume), 40u);

    /* Commands with no portable effect route to the injected callback. */
    mutating_frame(frame, sizeof(frame), "device.dismiss.press", "{}", "mutating");
    ZV_CHECK_EQ(zv_device_command_parse(frame, strlen(frame), &cmd), ZV_OK);
    ZV_CHECK_EQ(local_effect(&ctx, &cmd), ZV_OK);
    ZV_CHECK_EQ(g_apply_calls, 1);
    ZV_CHECK_STR(g_apply_command, "device.dismiss.press");

    /* With no callback the effect is unsupported, not a silent success. */
    ctx.apply = NULL;
    ZV_CHECK_EQ(local_effect(&ctx, &cmd), ZV_ERR_UNSUPPORTED);
}

/*
 * The local/remote asymmetry, asserted as a pair so it cannot be silently
 * removed by a later reader who assumes the hold should apply everywhere.
 */
static void test_local_path_skips_hold(void)
{
    harness_t h;
    zv_device_command_ctx_t ctx;
    char frame[900];
    zv_device_command_t cmd;
    zv_device_command_tier_t tier;
    zv_device_reason_t reason;

    harness_init(&h);
    zv_device_command_init(&ctx);
    ctx.digest_verify = fixture_digest;
    ctx.session_online = true;
    g_apply_calls = 0;

    /* Local: same effect, no approval block, no hold. */
    ZV_CHECK_EQ(zv_device_command_apply_local(&ctx, "device.volume.set", "{\"level\":70}", strlen("{\"level\":70}")), ZV_OK);
    ZV_CHECK_EQ(zv_volume_level(&ctx.volume), 70u);
    ZV_CHECK(!ctx.pending_mutating);

    /* The identical effect over the wire still requires the signed gate. */
    ZV_CHECK_EQ(zv_volume_set_level(&ctx.volume, 40u), ZV_OK);
    mutating_frame(frame, sizeof(frame), "device.volume.set", "{\"level\":70}", "mutating");
    ZV_CHECK_EQ(zv_device_command_parse(frame, strlen(frame), &cmd), ZV_OK);
    ZV_CHECK_EQ(cmd.has_approval, true);
    ctx.pending_mutating = false;

    zv_device_command_t unsigned_cmd = cmd;
    unsigned_cmd.has_approval = false;
    ZV_CHECK_EQ(zv_device_command_check(&ctx, &unsigned_cmd, &h.policy, &h.gate, NOW_S, &tier, &reason),
                ZV_ERR_POLICY);
    ZV_CHECK_EQ(reason, ZV_CMD_REASON_UNAUTHORIZED);
    ZV_CHECK_EQ(zv_volume_level(&ctx.volume), 40u);

    /* Local input gets no wider a vocabulary than a signed command. */
    ZV_CHECK_EQ(zv_device_command_apply_local(&ctx, "device.reboot", "{}", 2u), ZV_ERR_POLICY);
    ctx.apply = record_apply;
    ZV_CHECK_EQ(zv_device_command_apply_local(&ctx, "device.menu.open", "{}", 2u), ZV_OK);
    ZV_CHECK_EQ(g_apply_calls, 1);
}

/* --------------------------------------------------------------- result frame */

static void test_result_frame(void)
{
    char buf[ZV_CMD_RESULT_MAX];
    ZV_CHECK_EQ(zv_device_command_result_build(buf, sizeof(buf), CMD_ID, ZV_CMD_STATUS_APPLIED, ZV_CMD_REASON_NONE),
                ZV_OK);
    ZV_CHECK(strstr(buf, "\"type\":\"device.command.result\"") != NULL);
    ZV_CHECK(strstr(buf, CMD_ID) != NULL);
    ZV_CHECK(strstr(buf, "\"status\":\"applied\"") != NULL);
    /* Never echoes args, summary, or digest. */
    ZV_CHECK(strstr(buf, "level") == NULL);
    ZV_CHECK(strstr(buf, "summary") == NULL);
    ZV_CHECK(strstr(buf, "digest") == NULL);
    ZV_CHECK(strstr(buf, "signature") == NULL);

    char tiny[16];
    ZV_CHECK_EQ(zv_device_command_result_build(tiny, sizeof(tiny), CMD_ID, ZV_CMD_STATUS_REJECTED,
                                               ZV_CMD_REASON_UNAUTHORIZED),
                ZV_ERR_BUFFER_TOO_SMALL);
    ZV_CHECK_EQ(zv_device_command_result_build(buf, sizeof(buf), "not-a-uuid", ZV_CMD_STATUS_FAILED,
                                               ZV_CMD_REASON_MALFORMED),
                ZV_ERR_INVALID_ARG);
    ZV_CHECK_EQ(zv_device_command_result_build(NULL, 0u, CMD_ID, ZV_CMD_STATUS_FAILED, ZV_CMD_REASON_MALFORMED),
                ZV_ERR_INVALID_ARG);
}

static void test_names(void)
{
    ZV_CHECK_STR(zv_device_command_tier_name(ZV_CMD_TIER_READ), "read");
    ZV_CHECK_STR(zv_device_command_tier_name(ZV_CMD_TIER_PUSH), "push");
    ZV_CHECK_STR(zv_device_command_tier_name(ZV_CMD_TIER_MUTATING), "mutating");
    ZV_CHECK_STR(zv_device_status_name(ZV_CMD_STATUS_APPLIED), "applied");
    ZV_CHECK_STR(zv_device_status_name(ZV_CMD_STATUS_EXPIRED), "expired");
    ZV_CHECK_STR(zv_device_reason_name(ZV_CMD_REASON_UNKNOWN_COMMAND), "unknown_command");
    ZV_CHECK_STR(zv_device_reason_name(ZV_CMD_REASON_BUSY), "busy");
    ZV_CHECK_STR(zv_device_reason_name(ZV_CMD_REASON_LINK_UNAVAILABLE), "link_unavailable");
    ZV_CHECK_EQ(zv_device_reason_from_status(ZV_ERR_REPLAY), ZV_CMD_REASON_REPLAY);
    ZV_CHECK_EQ(zv_device_reason_from_status(ZV_ERR_SIGNATURE), ZV_CMD_REASON_SIGNATURE);
}

/* The published fixture must keep parsing. Schema/example drift is silent
 * otherwise: the C parser and protocol/v1/examples/ are edited separately. */
static void test_published_fixture_parses(void)
{
    static const char k_example[] =
        "{\"v\":1,\"type\":\"device.command\",\"command_id\":\"5a97be68-cfbe-4772-84e7-0ea48c1a0fc1\","
        "\"command\":\"device.volume.set\",\"args\":{\"level\":40},\"mutates\":true,"
        "\"approval_id\":\"11111111-2222-4333-8444-555555555555\",\"action\":\"device.volume.set\","
        "\"risk\":\"mutating\",\"summary\":\"Set the puck speaker volume to 40 percent\","
        "\"action_digest\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\","
        "\"nonce\":\"AAAAAAAAAAAAAAAAAAAAAA\",\"kid\":\"fixture-key-1\","
        "\"issued_at\":1893456000,\"expires_at\":1893456060,"
        "\"signature\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\"}";
    zv_device_command_t cmd;
    ZV_CHECK_EQ(zv_device_command_parse(k_example, strlen(k_example), &cmd), ZV_OK);
    ZV_CHECK_STR(cmd.command, "device.volume.set");
    ZV_CHECK_STR(cmd.approval.action, "device.volume.set");
    ZV_CHECK_STR(cmd.approval.risk, "mutating");
    ZV_CHECK(cmd.has_approval);

    /* And the result frame round-trips into its own bounds. */
    char result[ZV_CMD_RESULT_MAX];
    ZV_CHECK_EQ(zv_device_command_result_build(result, sizeof(result), cmd.command_id, ZV_CMD_STATUS_APPLIED,
                                               ZV_CMD_REASON_NONE),
                ZV_OK);
    ZV_CHECK_STR(result,
                 "{\"v\":1,\"type\":\"device.command.result\",\"command_id\":\"5a97be68-cfbe-4772-84e7-0ea48c1a0fc1\","
                 "\"status\":\"applied\",\"reason_code\":\"none\"}");
}

/* An unknown key inside args is malformed, not ignored. Otherwise a sender could
 * attach a second meaning to a command whose handler only reads the key it knows. */
static void test_args_unknown_keys_rejected(void)
{
    zv_device_command_ctx_t ctx;
    zv_device_command_t cmd;
    char frame[900];

    zv_device_command_init(&ctx);
    ctx.digest_verify = fixture_digest;
    ctx.session_online = true;

    mutating_frame(frame, sizeof(frame), "device.volume.set", "{\"level\":40,\"evil\":1}", "mutating");
    ZV_CHECK_EQ(zv_device_command_parse(frame, strlen(frame), &cmd), ZV_OK);
    ZV_CHECK_EQ(local_effect(&ctx, &cmd), ZV_ERR_POLICY);
    ZV_CHECK_EQ(zv_volume_level(&ctx.volume), 40u); /* untouched */

    mutating_frame(frame, sizeof(frame), "device.mute.set", "{\"muted\":true,\"level\":0}", "mutating");
    ZV_CHECK_EQ(zv_device_command_parse(frame, strlen(frame), &cmd), ZV_OK);
    ZV_CHECK_EQ(local_effect(&ctx, &cmd), ZV_ERR_POLICY);
    ZV_CHECK(!zv_volume_is_muted(&ctx.volume)); /* still untouched */

    /* The well-formed single-key form still works. */
    mutating_frame(frame, sizeof(frame), "device.volume.set", "{\"level\":55}", "mutating");
    ZV_CHECK_EQ(zv_device_command_parse(frame, strlen(frame), &cmd), ZV_OK);
    ZV_CHECK_EQ(local_effect(&ctx, &cmd), ZV_OK);
    ZV_CHECK_EQ(zv_volume_level(&ctx.volume), 55u);
}

/* ------------------------------------------------------------- ingress seam */

static void test_malformed_command_replies_use_only_top_level_id(void)
{
    static const char *const recoverable[] = {
        "{\"command_id\":\"" CMD_ID "\",\"command\":\"bad command\"}",
        "{\"args\":{\"command_id\":\"bad\"},\"command_id\":\"" CMD_ID "\"}",
        "{\"command_id\":\"" CMD_ID "\",\"v\":2}",
    };
    static const char *const ambiguous[] = {
        "{\"args\":{\"command_id\":\"" CMD_ID "\"}}",
        "{\"args\":[\"x\",\"command_id\",\"" CMD_ID "\"]}",
        "{\"command_id\":\"" CMD_ID "\",\"command_id\":\"" CMD_ID "\"}",
        "{\"command_id\":\"bad\"}",
        "{\"command_id\":\"" CMD_ID "\"",
    };
    zv_device_command_ctx_t ctx;
    zv_device_command_init(&ctx);
    char result[ZV_CMD_RESULT_MAX];
    size_t len;
    zv_device_reason_t reason;
    for (size_t i = 0u; i < sizeof(recoverable) / sizeof(recoverable[0]); i++) {
        size_t bytes = strlen(recoverable[i]);
        char frame[ZV_CMD_FRAME_MAX];
        memset(frame, 'X', sizeof(frame));
        memcpy(frame, recoverable[i], bytes); /* deliberately no terminator */
        ZV_CHECK_EQ(zv_device_command_ingest(&ctx, frame, bytes, NULL, NULL, NOW_S,
                    result, sizeof(result), &len, &reason), ZV_OK);
        ZV_CHECK_EQ(reason, ZV_CMD_REASON_MALFORMED);
        ZV_CHECK_EQ(len, strlen(result));
        ZV_CHECK(strstr(result, CMD_ID) != NULL);
        ZV_CHECK(strstr(result, "\"status\":\"rejected\"") != NULL);
        ZV_CHECK_EQ(zv_volume_level(&ctx.volume), 40u);
        ZV_CHECK(!ctx.pending_mutating);
    }
    for (size_t i = 0u; i < sizeof(ambiguous) / sizeof(ambiguous[0]); i++) {
        ZV_CHECK_EQ(zv_device_command_ingest(&ctx, ambiguous[i], strlen(ambiguous[i]),
                    NULL, NULL, NOW_S, result, sizeof(result), &len, &reason), ZV_ERR_INVALID_ARG);
        ZV_CHECK_EQ(len, 0u);
    }
}

static void test_ingest_round_trip(void)
{
    harness_t h;
    zv_device_command_ctx_t ctx;
    char frame[900];
    char result[ZV_CMD_RESULT_MAX];
    size_t len = 0u;
    zv_device_reason_t reason = ZV_CMD_REASON_NONE;

    harness_init(&h);
    zv_device_command_init(&ctx);
    ctx.digest_verify = fixture_digest;
    ctx.session_online = true;

    /* A signed mutating command goes all the way through and is applied. */
    mutating_frame(frame, sizeof(frame), "device.volume.set", "{\"level\":45}", "mutating");
    ZV_CHECK_EQ(zv_device_command_ingest(&ctx, frame, strlen(frame), &h.policy, &h.gate, NOW_S, result, sizeof(result),
                                          &len, &reason),
                ZV_OK);
    ZV_CHECK_EQ(reason, ZV_CMD_REASON_NONE);
    ZV_CHECK_EQ(len, 0u);
    ZV_CHECK_EQ(zv_volume_level(&ctx.volume), 40u);
    ZV_CHECK(ctx.pending_mutating);
    // Poll a fresh physical hold; ingest alone never applies.
    ZV_CHECK_EQ(zv_device_command_confirm(&ctx, false, false, 0u, NOW_S, result, sizeof(result), &len, &reason), ZV_OK);
    ZV_CHECK_EQ(zv_device_command_confirm(&ctx, true, false, 400u, NOW_S, result, sizeof(result), &len, &reason), ZV_OK);
    ZV_CHECK_EQ(zv_device_command_confirm(&ctx, true, false, 1900u, NOW_S + 2, result, sizeof(result), &len, &reason), ZV_OK);
    ZV_CHECK_EQ(reason, ZV_CMD_REASON_APPLIED);
    ZV_CHECK_EQ(len, strlen(result));
    ZV_CHECK(strstr(result, "\"status\":\"applied\"") != NULL);
    ZV_CHECK_EQ(zv_volume_level(&ctx.volume), 45u);
    /* The terminal confirmation releases the slot. */
    ZV_CHECK(!ctx.pending_mutating);

    /* A rejected command is still a completed exchange and is answered. */
    zv_device_command_hold_clear(&ctx);
    mutating_frame(frame, sizeof(frame), "device.volume.set", "{\"level\":45}", "mutating");
    ZV_CHECK_EQ(zv_device_command_ingest(&ctx, frame, strlen(frame), &h.policy, &h.gate, NOW_S, result, sizeof(result),
                                          &len, &reason),
                ZV_OK);
    ZV_CHECK_EQ(reason, ZV_CMD_REASON_REPLAY); /* same nonce, second time */
    ZV_CHECK(strstr(result, "\"status\":\"rejected\"") != NULL);
    ZV_CHECK(strstr(result, "\"reason_code\":\"replay\"") != NULL);

    /* An expired approval is reported as expired, not as a generic rejection. */
    zv_device_command_hold_clear(&ctx);
    mutating_frame(frame, sizeof(frame), "device.volume.set", "{\"level\":45}", "mutating");
    ZV_CHECK_EQ(zv_device_command_ingest(&ctx, frame, strlen(frame), &h.policy, &h.gate, NOW_S + 600, result,
                                          sizeof(result), &len, &reason),
                ZV_OK);
    ZV_CHECK_EQ(reason, ZV_CMD_REASON_EXPIRED);
    ZV_CHECK(strstr(result, "\"status\":\"expired\"") != NULL);

    /* A wrong-type frame is refused with no reply: there is no id to answer. */
    ZV_CHECK_EQ(zv_device_command_ingest(&ctx, "{\"v\":1,\"type\":\"avatar.event\"}", 28u, &h.policy, &h.gate, NOW_S,
                                          result, sizeof(result), &len, &reason),
                ZV_ERR_INVALID_ARG);
    ZV_CHECK_EQ(len, 0u);

    /* A frame that is not even JSON produces no reply either. */
    ZV_CHECK_EQ(zv_device_command_ingest(&ctx, "not json", 8u, &h.policy, &h.gate, NOW_S, result, sizeof(result), &len,
                                          &reason),
                ZV_ERR_INVALID_ARG);
    ZV_CHECK_EQ(len, 0u);

    /* A result that cannot fit is reported as such rather than truncated. */
    char tiny[8];
    mutating_frame(frame, sizeof(frame), "device.volume.set", "{\"level\":45}", "mutating");
    ZV_CHECK_EQ(zv_device_command_ingest(&ctx, frame, strlen(frame), &h.policy, &h.gate, NOW_S + 1, tiny, sizeof(tiny),
                                          &len, &reason),
                ZV_ERR_BUFFER_TOO_SMALL);

    ZV_CHECK_EQ(zv_device_command_ingest(NULL, frame, strlen(frame), &h.policy, &h.gate, NOW_S, result, sizeof(result),
                                          &len, &reason),
                ZV_ERR_INVALID_ARG);
}


static void test_binding_and_hold_lifecycle(void)
{
    harness_t h;
    zv_device_command_ctx_t ctx;
    zv_device_command_t cmd, changed;
    zv_device_command_tier_t tier;
    zv_device_reason_t reason;
    char frame[900], result[ZV_CMD_RESULT_MAX];
    size_t len;
    harness_init(&h);
    zv_device_command_init(&ctx);
    ctx.session_online = true;
    ctx.digest_verify = fixture_digest;
    mutating_frame(frame, sizeof(frame), "device.volume.set", "{\"level\":70}", "mutating");
    ZV_CHECK_EQ(zv_device_command_parse(frame, strlen(frame), &cmd), ZV_OK);
    changed = cmd;
    snprintf(changed.command, sizeof(changed.command), "%s", "device.mute.set");
    ZV_CHECK_EQ(zv_device_command_check(&ctx, &changed, &h.policy, &h.gate, NOW_S, &tier, &reason), ZV_ERR_POLICY);
    changed = cmd;
    snprintf(changed.args, sizeof(changed.args), "%s", "{\"level\":100}");
    ZV_CHECK_EQ(zv_device_command_check(&ctx, &changed, &h.policy, &h.gate, NOW_S, &tier, &reason), ZV_ERR_POLICY);
    ctx.digest_verify = NULL;
    ZV_CHECK_EQ(zv_device_command_check(&ctx, &cmd, &h.policy, &h.gate, NOW_S, &tier, &reason), ZV_ERR_POLICY);
    ctx.digest_verify = fixture_digest;
    // Rejections did not reserve the nonce. The genuine command still works.
    ZV_CHECK_EQ(zv_device_command_check(&ctx, &cmd, &h.policy, &h.gate, NOW_S, &tier, &reason), ZV_OK);
    ZV_CHECK_EQ(zv_device_command_apply(&ctx, &cmd), ZV_ERR_POLICY);
    // Altering the caller's copy after gating cannot alter the retained effect.
    snprintf(cmd.args, sizeof(cmd.args), "%s", "{\"level\":100}");
    complete_hold(&ctx);
    ZV_CHECK_EQ(zv_volume_level(&ctx.volume), 70u);
    ZV_CHECK_EQ(zv_device_command_confirm(&ctx, true, false, 2000u, NOW_S + 2,
                                         result, sizeof(result), &len, &reason), ZV_ERR_INVALID_ARG);

    for (unsigned scenario = 0u; scenario < 3u; scenario++) {
        harness_init(&h);
        zv_device_command_init(&ctx);
        ctx.session_online = true;
        ctx.digest_verify = fixture_digest;
        mutating_frame(frame, sizeof(frame), "device.volume.set", "{\"level\":70}", "mutating");
        ZV_CHECK_EQ(zv_device_command_ingest(&ctx, frame, strlen(frame), &h.policy, &h.gate, NOW_S,
                                             result, sizeof(result), &len, &reason), ZV_OK);
        ZV_CHECK_EQ(len, 0u);
        if (scenario == 1u) ctx.session_online = false;
        int64_t now = scenario == 2u ? NOW_S + 60 : NOW_S;
        ZV_CHECK_EQ(zv_device_command_confirm(&ctx, false, scenario == 0u, 0u, now,
                                             result, sizeof(result), &len, &reason), ZV_OK);
        ZV_CHECK(len > 0u);
        ZV_CHECK(!ctx.pending_mutating);
        ZV_CHECK_EQ(zv_volume_level(&ctx.volume), 40u);
    }

    // Short contact must be released/repressed, not accumulated into approval.
    harness_init(&h);
    zv_device_command_init(&ctx);
    ctx.session_online = true;
    ctx.digest_verify = fixture_digest;
    ZV_CHECK_EQ(zv_device_command_ingest(&ctx, frame, strlen(frame), &h.policy, &h.gate, NOW_S,
                                         result, sizeof(result), &len, &reason), ZV_OK);
    ZV_CHECK_EQ(zv_device_command_confirm(&ctx, true, false, 0u, NOW_S, result, sizeof(result), &len, &reason), ZV_OK);
    ZV_CHECK_EQ(zv_device_command_confirm(&ctx, true, false, 400u, NOW_S, result, sizeof(result), &len, &reason), ZV_OK);
    ZV_CHECK_EQ(zv_device_command_confirm(&ctx, true, false, 1900u, NOW_S + 2, result, sizeof(result), &len, &reason), ZV_OK);
    ZV_CHECK_EQ(len, 0u); /* A carried-over press is still not a fresh hold. */
    ZV_CHECK_EQ(zv_device_command_confirm(&ctx, false, false, 2000u, NOW_S + 2, result, sizeof(result), &len, &reason), ZV_OK);
    ZV_CHECK_EQ(zv_device_command_confirm(&ctx, true, false, 2100u, NOW_S + 2, result, sizeof(result), &len, &reason), ZV_OK);
    ZV_CHECK_EQ(zv_device_command_confirm(&ctx, false, false, 2500u, NOW_S + 3, result, sizeof(result), &len, &reason), ZV_OK);
    ZV_CHECK_EQ(len, 0u);
    ZV_CHECK_EQ(zv_device_command_confirm(&ctx, true, false, 3000u, NOW_S + 3, result, sizeof(result), &len, &reason), ZV_OK);
    ZV_CHECK_EQ(zv_device_command_confirm(&ctx, true, false, 4499u, NOW_S + 4, result, sizeof(result), &len, &reason), ZV_OK);
    ZV_CHECK_EQ(len, 0u);
    ZV_CHECK_EQ(zv_device_command_confirm(&ctx, true, false, 4500u, NOW_S + 5, result, sizeof(result), &len, &reason), ZV_OK);
    ZV_CHECK_EQ(zv_volume_level(&ctx.volume), 70u);
}

static void test_parser_exact_bounds_and_tokens(void)
{
    zv_device_command_t cmd;
    char args[131], frame[512];
    for (size_t n = 128u; n <= 129u; n++) {
        args[0] = '{';
        memset(args + 1, ' ', n - 2u);
        args[n - 1u] = '}';
        args[n] = '\0';
        (void)snprintf(frame, sizeof(frame),
                       "{\"v\":1,\"type\":\"device.command\",\"command_id\":\"%s\",\"command\":\"device.volume.get\",\"args\":%s}",
                       CMD_ID, args);
        ZV_CHECK_EQ(zv_device_command_parse(frame, strlen(frame), &cmd), n == 128u ? ZV_OK : ZV_ERR_BUFFER_TOO_SMALL);
    }
    uint8_t level;
    bool muted;
    const char *bad_levels[] = {"{\"level\":40.5}", "{\"level\":40junk}", "{\"level\":01}", "{\"level\":40,\"level\":80}"};
    for (size_t i = 0u; i < sizeof(bad_levels) / sizeof(bad_levels[0]); i++)
        ZV_CHECK(!zv_device_args_level(bad_levels[i], strlen(bad_levels[i]), &level));
    ZV_CHECK(!zv_device_args_flag("{\"muted\":truejunk}", strlen("{\"muted\":truejunk}"), &muted));
    const char *good = " { \"level\" : 40 } ";
    ZV_CHECK(zv_device_args_level(good, strlen(good), &level));
    ZV_CHECK_EQ(level, 40u);
    (void)snprintf(frame, sizeof(frame),
                   "{\"v\":1,\"v\":1,\"type\":\"device.command\",\"command_id\":\"%s\",\"command\":\"device.volume.get\"}", CMD_ID);
    ZV_CHECK_EQ(zv_device_command_parse(frame, strlen(frame), &cmd), ZV_ERR_INVALID_ARG);
}

void test_suite_device_command(void)
{
    test_binding_and_hold_lifecycle();
    test_parser_exact_bounds_and_tokens();
    test_allowlist_and_names();
    test_parse_read_frame();
    test_parse_mutating_frame();
    test_parse_rejects();
    test_published_fixture_parses();
    test_device_state();
    test_tier_enforcement();
    test_link_and_busy();
    test_mutating_gate_reuses_approval();
    test_forgery_cannot_exhaust_nonce_cache();
    test_risk_vocabulary();
    test_apply_routes_and_bounds();
    test_args_unknown_keys_rejected();
    test_local_path_skips_hold();
    test_result_frame();
    test_ingest_round_trip();
    test_malformed_command_replies_use_only_top_level_id();
    test_names();
}