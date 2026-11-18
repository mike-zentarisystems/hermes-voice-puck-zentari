/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 *
 * A/B slot lifecycle and post-download digest comparison.
 *
 * The point of these tests is the negative path: an image that was truncated,
 * overlong, or digest-mismatched must never be able to reach CONFIRMED, and a
 * failed boot must roll back rather than sit pending.
 */
#include "zv_identity.h"
#include "zv_ota_policy.h"
#include "zv_test.h"

/* Deliberately larger than ZV_OTA_SLOT_CHUNK_MAX so no image can arrive in a
 * single write, and a chunked transfer is exercised on every path. */
#define IMAGE_SIZE 5000u

static uint8_t digest_of(uint8_t seed)
{
    uint8_t v = seed;
    for (int i = 0; i < 32; i++) {
        v = (uint8_t)((v * 31u) + 7u);
    }
    return v;
}

static void fill_digest(uint8_t out[32], uint8_t seed)
{
    for (size_t i = 0u; i < 32u; i++) {
        out[i] = digest_of((uint8_t)(seed + i));
    }
}

static void hex_of(const uint8_t digest[32], char out[ZV_OTA_SHA256_LEN + 1u])
{
    static const char k_hex[] = "0123456789abcdef";
    for (size_t i = 0u; i < 32u; i++) {
        out[i * 2u] = k_hex[(digest[i] >> 4) & 0x0Fu];
        out[(i * 2u) + 1u] = k_hex[digest[i] & 0x0Fu];
    }
    out[ZV_OTA_SHA256_LEN] = '\0';
}

/** Writes exactly expected_size bytes using only legal chunks. */
static zv_status_t write_all(zv_ota_slot_t *slot, uint32_t expected_size)
{
    uint32_t remaining = expected_size;
    while (remaining > 0u) {
        uint32_t chunk = (remaining > ZV_OTA_SLOT_CHUNK_MAX) ? ZV_OTA_SLOT_CHUNK_MAX : remaining;
        zv_status_t status = zv_ota_slot_write(slot, chunk);
        if (status != ZV_OK) {
            return status;
        }
        remaining -= chunk;
    }
    return ZV_OK;
}

/** Drive a slot to AWAITING_CONFIRM, the only state that may confirm or roll back. */
static zv_status_t drive_to_awaiting(zv_ota_slot_t *slot, const uint8_t digest[32])
{
    zv_status_t status = zv_ota_slot_begin(slot, IMAGE_SIZE);
    if (status != ZV_OK) {
        return status;
    }
    status = write_all(slot, IMAGE_SIZE);
    if (status != ZV_OK) {
        return status;
    }
    status = zv_ota_slot_finish_download(slot);
    if (status != ZV_OK) {
        return status;
    }
    char hex[ZV_OTA_SHA256_LEN + 1u];
    hex_of(digest, hex);
    return zv_ota_slot_digest_result(slot, zv_ota_digest_equal(hex, digest, 32u));
}

/* ------------------------------------------------------------ digest compare */

static void test_digest_equal(void)
{
    uint8_t digest[32];
    char hex[ZV_OTA_SHA256_LEN + 1u];
    fill_digest(digest, 1u);
    hex_of(digest, hex);

    ZV_CHECK(zv_ota_digest_equal(hex, digest, 32u));

    /* One flipped bit anywhere must be detected. */
    for (size_t i = 0u; i < 32u; i++) {
        uint8_t saved = digest[i];
        digest[i] = (uint8_t)(saved ^ 0x01u);
        ZV_CHECK(!zv_ota_digest_equal(hex, digest, 32u));
        digest[i] = saved;
    }

    /* Wrong raw length, nulls, and non-canonical hex all fail closed. */
    ZV_CHECK(!zv_ota_digest_equal(hex, digest, 31u));
    ZV_CHECK(!zv_ota_digest_equal(hex, digest, 33u));
    ZV_CHECK(!zv_ota_digest_equal(NULL, digest, 32u));
    ZV_CHECK(!zv_ota_digest_equal(hex, NULL, 32u));
    ZV_CHECK(!zv_ota_digest_equal("short", digest, 32u));
    ZV_CHECK(!zv_ota_digest_equal("ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789", digest, 32u));
}

/* -------------------------------------------------------------- slot lifecycle */

static void test_slot_happy_path(void)
{
    uint8_t digest[32];
    char hex[ZV_OTA_SHA256_LEN + 1u];
    fill_digest(digest, 2u);
    hex_of(digest, hex);

    zv_ota_slot_t slot;
    zv_ota_slot_init(&slot);
    ZV_CHECK_EQ(slot.state, ZV_OTA_SLOT_IDLE);

    ZV_CHECK_EQ(zv_ota_slot_begin(&slot, IMAGE_SIZE), ZV_OK);
    ZV_CHECK_EQ(slot.state, ZV_OTA_SLOT_DOWNLOADING);
    ZV_CHECK_EQ(slot.expected_size, IMAGE_SIZE);

    ZV_CHECK_EQ(write_all(&slot, IMAGE_SIZE), ZV_OK);
    ZV_CHECK_EQ(slot.received, IMAGE_SIZE);

    ZV_CHECK_EQ(zv_ota_slot_finish_download(&slot), ZV_OK);
    ZV_CHECK_EQ(slot.state, ZV_OTA_SLOT_VERIFYING);

    ZV_CHECK_EQ(zv_ota_slot_digest_result(&slot, zv_ota_digest_equal(hex, digest, 32u)), ZV_OK);
    ZV_CHECK_EQ(slot.state, ZV_OTA_SLOT_AWAITING_CONFIRM);

    ZV_CHECK_EQ(zv_ota_slot_boot_ok(&slot), ZV_OK);
    ZV_CHECK_EQ(slot.state, ZV_OTA_SLOT_CONFIRMED);
}

static void test_slot_rejects_illegal_shortcuts(void)
{
    zv_ota_slot_t slot;
    zv_ota_slot_init(&slot);

    /* Nothing may be confirmed or rolled back before a download even starts. */
    ZV_CHECK_EQ(zv_ota_slot_boot_ok(&slot), ZV_ERR_INVALID_STATE);
    ZV_CHECK_EQ(zv_ota_slot_boot_failed(&slot), ZV_ERR_INVALID_STATE);
    ZV_CHECK_EQ(zv_ota_slot_finish_download(&slot), ZV_ERR_INVALID_STATE);
    ZV_CHECK_EQ(zv_ota_slot_digest_result(&slot, true), ZV_ERR_INVALID_STATE);
    ZV_CHECK_EQ(zv_ota_slot_write(&slot, 10u), ZV_ERR_INVALID_STATE);

    ZV_CHECK_EQ(zv_ota_slot_begin(&slot, IMAGE_SIZE), ZV_OK);
    /* A digest verdict cannot be manufactured before the bytes arrive. */
    ZV_CHECK_EQ(zv_ota_slot_digest_result(&slot, true), ZV_ERR_INVALID_STATE);
    ZV_CHECK_EQ(zv_ota_slot_boot_ok(&slot), ZV_ERR_INVALID_STATE);

    /* A truncated transfer must not reach the digest stage. */
    ZV_CHECK_EQ(zv_ota_slot_write(&slot, ZV_OTA_SLOT_CHUNK_MAX), ZV_OK);
    ZV_CHECK_EQ(slot.received, ZV_OTA_SLOT_CHUNK_MAX);
    ZV_CHECK_EQ(zv_ota_slot_finish_download(&slot), ZV_ERR_POLICY);
    ZV_CHECK_EQ(slot.state, ZV_OTA_SLOT_FAILED);
    ZV_CHECK_EQ(slot.reason, ZV_OTA_REASON_BAD_MANIFEST);

    /* Even after a failure the slot cannot be confirmed. */
    ZV_CHECK_EQ(zv_ota_slot_boot_ok(&slot), ZV_ERR_INVALID_STATE);
    ZV_CHECK_EQ(zv_ota_slot_digest_result(&slot, true), ZV_ERR_INVALID_STATE);
}

static void test_slot_bounds_writes(void)
{
    zv_ota_slot_t slot;
    zv_ota_slot_init(&slot);
    ZV_CHECK_EQ(zv_ota_slot_begin(&slot, IMAGE_SIZE), ZV_OK);

    /* An empty chunk and an oversized chunk are argument errors, not policy. */
    ZV_CHECK_EQ(zv_ota_slot_write(&slot, 0u), ZV_ERR_INVALID_ARG);
    ZV_CHECK_EQ(zv_ota_slot_write(&slot, ZV_OTA_SLOT_CHUNK_MAX + 1u), ZV_ERR_INVALID_ARG);
    ZV_CHECK_EQ(slot.state, ZV_OTA_SLOT_DOWNLOADING);

    /* A legal chunk that would overrun the accepted manifest fails closed. */
    ZV_CHECK_EQ(zv_ota_slot_write(&slot, ZV_OTA_SLOT_CHUNK_MAX), ZV_OK);
    ZV_CHECK_EQ(slot.received, ZV_OTA_SLOT_CHUNK_MAX);
    ZV_CHECK_EQ(zv_ota_slot_write(&slot, 1000u), ZV_ERR_POLICY);
    ZV_CHECK_EQ(slot.state, ZV_OTA_SLOT_FAILED);
    ZV_CHECK_EQ(slot.reason, ZV_OTA_REASON_TOO_LARGE);
    /* The rejected chunk was not counted. */
    ZV_CHECK_EQ(slot.received, ZV_OTA_SLOT_CHUNK_MAX);

    ZV_CHECK_EQ(zv_ota_slot_begin(NULL, IMAGE_SIZE), ZV_ERR_INVALID_ARG);
    ZV_CHECK_EQ(zv_ota_slot_begin(&slot, 0u), ZV_ERR_INVALID_ARG);
    ZV_CHECK_EQ(zv_ota_slot_write(NULL, 10u), ZV_ERR_INVALID_ARG);
    ZV_CHECK_EQ(zv_ota_slot_finish_download(NULL), ZV_ERR_INVALID_ARG);
    ZV_CHECK_EQ(zv_ota_slot_digest_result(NULL, true), ZV_ERR_INVALID_ARG);
    ZV_CHECK_EQ(zv_ota_slot_boot_ok(NULL), ZV_ERR_INVALID_ARG);
    ZV_CHECK_EQ(zv_ota_slot_boot_failed(NULL), ZV_ERR_INVALID_ARG);
}

static void test_slot_digest_mismatch_is_not_confirmable(void)
{
    uint8_t digest[32];
    char hex[ZV_OTA_SHA256_LEN + 1u];
    fill_digest(digest, 3u);
    hex_of(digest, hex);

    zv_ota_slot_t slot;
    zv_ota_slot_init(&slot);
    ZV_CHECK_EQ(zv_ota_slot_begin(&slot, IMAGE_SIZE), ZV_OK);
    ZV_CHECK_EQ(write_all(&slot, IMAGE_SIZE), ZV_OK);
    ZV_CHECK_EQ(zv_ota_slot_finish_download(&slot), ZV_OK);

    /* A digest that does not match the manifest must refuse the update. */
    uint8_t corrupt[32];
    memcpy(corrupt, digest, sizeof(corrupt));
    corrupt[31] = (uint8_t)(corrupt[31] ^ 0xFFu);
    ZV_CHECK(!zv_ota_digest_equal(hex, corrupt, 32u));
    ZV_CHECK_EQ(zv_ota_slot_digest_result(&slot, false), ZV_ERR_POLICY);
    ZV_CHECK_EQ(slot.state, ZV_OTA_SLOT_FAILED);
    ZV_CHECK_EQ(slot.reason, ZV_OTA_REASON_DIGEST_MISMATCH);

    /* The failed slot is not a valid image, so it can never be confirmed. */
    ZV_CHECK_EQ(zv_ota_slot_boot_ok(&slot), ZV_ERR_INVALID_STATE);
    ZV_CHECK_EQ(zv_ota_slot_boot_failed(&slot), ZV_ERR_INVALID_STATE);
}

static void test_slot_rolls_back_failed_self_test(void)
{
    /* A verified image that boots but fails its self-test must roll back. */
    uint8_t digest[32];
    fill_digest(digest, 9u);

    zv_ota_slot_t rolled;
    zv_ota_slot_init(&rolled);
    ZV_CHECK_EQ(drive_to_awaiting(&rolled, digest), ZV_OK);
    ZV_CHECK_EQ(rolled.state, ZV_OTA_SLOT_AWAITING_CONFIRM);

    ZV_CHECK_EQ(zv_ota_slot_boot_failed(&rolled), ZV_OK);
    ZV_CHECK_EQ(rolled.state, ZV_OTA_SLOT_ROLLED_BACK);
    /* A rolled-back slot is terminal: it cannot also be marked healthy. */
    ZV_CHECK_EQ(zv_ota_slot_boot_ok(&rolled), ZV_ERR_INVALID_STATE);

    /* The same path with a good self-test reaches CONFIRMED instead. */
    zv_ota_slot_t confirmed;
    zv_ota_slot_init(&confirmed);
    ZV_CHECK_EQ(drive_to_awaiting(&confirmed, digest), ZV_OK);
    ZV_CHECK_EQ(zv_ota_slot_boot_ok(&confirmed), ZV_OK);
    ZV_CHECK_EQ(confirmed.state, ZV_OTA_SLOT_CONFIRMED);
    /* A confirmed slot is terminal too. */
    ZV_CHECK_EQ(zv_ota_slot_boot_failed(&confirmed), ZV_ERR_INVALID_STATE);
}

static void test_slot_retry_after_failure(void)
{
    uint8_t digest[32];
    fill_digest(digest, 5u);

    zv_ota_slot_t slot;
    zv_ota_slot_init(&slot);
    ZV_CHECK_EQ(zv_ota_slot_begin(&slot, IMAGE_SIZE), ZV_OK);
    ZV_CHECK_EQ(zv_ota_slot_write(&slot, 10u), ZV_OK);
    ZV_CHECK_EQ(zv_ota_slot_finish_download(&slot), ZV_ERR_POLICY); /* short */
    ZV_CHECK_EQ(slot.state, ZV_OTA_SLOT_FAILED);

    /* Retrying after a failure is allowed and starts from a clean slate. */
    ZV_CHECK_EQ(zv_ota_slot_begin(&slot, IMAGE_SIZE), ZV_OK);
    ZV_CHECK_EQ(slot.received, 0u);
    ZV_CHECK_EQ(slot.reason, ZV_OTA_REASON_NONE);
    ZV_CHECK_EQ(drive_to_awaiting(&slot, digest), ZV_ERR_INVALID_STATE); /* begin twice */

    zv_ota_slot_t clean;
    zv_ota_slot_init(&clean);
    ZV_CHECK_EQ(drive_to_awaiting(&clean, digest), ZV_OK);
    ZV_CHECK_EQ(clean.state, ZV_OTA_SLOT_AWAITING_CONFIRM);

    /* An update already in progress cannot be silently restarted. */
    ZV_CHECK_EQ(zv_ota_slot_begin(&clean, IMAGE_SIZE), ZV_ERR_INVALID_STATE);
}

static void test_slot_state_names(void)
{
    ZV_CHECK_STR(zv_ota_slot_state_name(ZV_OTA_SLOT_IDLE), "idle");
    ZV_CHECK_STR(zv_ota_slot_state_name(ZV_OTA_SLOT_AWAITING_CONFIRM), "awaiting_confirm");
    ZV_CHECK_STR(zv_ota_slot_state_name(ZV_OTA_SLOT_ROLLED_BACK), "rolled_back");
    ZV_CHECK_STR(zv_ota_slot_state_name((zv_ota_slot_state_t)99), "unknown");
    ZV_CHECK_STR(zv_ota_reason_name(ZV_OTA_REASON_DIGEST_MISMATCH), "digest_mismatch");
    ZV_CHECK_STR(zv_ota_reason_name((zv_ota_reason_t)99), "unknown");
}

/** The policy gate and the slot machine must agree on the board and size. */
static void test_slot_follows_manifest_policy(void)
{
    zv_ota_manifest_t manifest;
    memset(&manifest, 0, sizeof(manifest));
    snprintf(manifest.version, sizeof(manifest.version), "0.3.0");
    snprintf(manifest.board_id, sizeof(manifest.board_id), "%s", ZV_BOARD_ID);
    snprintf(manifest.channel, sizeof(manifest.channel), "stable");
    snprintf(manifest.sha256, sizeof(manifest.sha256),
             "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08");
    snprintf(manifest.signature_scheme, sizeof(manifest.signature_scheme), "%s", ZV_OTA_SIG_SCHEME_SECURE_BOOT_V2);
    manifest.secure_version = 2u;
    manifest.image_size = IMAGE_SIZE;

    const zv_ota_device_t device = {
        .running_version = "0.2.0",
        .board_id = ZV_BOARD_ID,
        .channel = "stable",
        .efuse_secure_version = 2u,
        .partition_size = IMAGE_SIZE,
    };
    zv_ota_reason_t reason = ZV_OTA_REASON_NONE;
    ZV_CHECK_EQ(zv_ota_evaluate(&manifest, &device, &reason), ZV_OK);

    /* The accepted manifest size is exactly what the slot will accept. */
    zv_ota_slot_t slot;
    zv_ota_slot_init(&slot);
    ZV_CHECK_EQ(zv_ota_slot_begin(&slot, manifest.image_size), ZV_OK);
    ZV_CHECK_EQ(write_all(&slot, manifest.image_size), ZV_OK);
    ZV_CHECK_EQ(zv_ota_slot_finish_download(&slot), ZV_OK);
    /* One byte past the accepted manifest is refused. */
    ZV_CHECK_EQ(zv_ota_slot_write(&slot, 1u), ZV_ERR_INVALID_STATE);
}

/**
 * The probation window: an image that boots and never proves itself must be
 * rolled back rather than left pending forever. The bound and the rule come
 * from the Hermes Gadget SDK, which boots a new image on probation and reverts
 * when it does not reach its server in time.
 */
static void test_slot_probation_window(void)
{
    zv_ota_slot_t slot;
    zv_ota_slot_init(&slot);
    ZV_CHECK_EQ(zv_ota_slot_begin(&slot, IMAGE_SIZE), ZV_OK);
    ZV_CHECK_EQ(write_all(&slot, IMAGE_SIZE), ZV_OK);
    ZV_CHECK_EQ(zv_ota_slot_finish_download(&slot), ZV_OK);
    ZV_CHECK_EQ(zv_ota_slot_digest_result(&slot, true), ZV_OK);

    ZV_CHECK_EQ(zv_ota_slot_begin_probation(&slot, 1000u), ZV_OK);
    ZV_CHECK(!zv_ota_slot_probation_expired(&slot, 1000u, ZV_OTA_PROBATION_MS_DEFAULT));
    ZV_CHECK(!zv_ota_slot_probation_expired(&slot, 1000u + ZV_OTA_PROBATION_MS_DEFAULT - 1u,
                                            ZV_OTA_PROBATION_MS_DEFAULT));
    ZV_CHECK(zv_ota_slot_probation_expired(&slot, 1000u + ZV_OTA_PROBATION_MS_DEFAULT,
                                           ZV_OTA_PROBATION_MS_DEFAULT));

    /* An expired probation is rolled back to the previous image. */
    ZV_CHECK_EQ(zv_ota_slot_boot_failed(&slot), ZV_OK);
    ZV_CHECK_EQ(slot.state, ZV_OTA_SLOT_ROLLED_BACK);
    /* A decided slot cannot expire a second time. */
    ZV_CHECK(!zv_ota_slot_probation_expired(&slot, 1000u + ZV_OTA_PROBATION_MS_DEFAULT * 2u,
                                            ZV_OTA_PROBATION_MS_DEFAULT));
}

/** Only an image that is awaiting confirmation has a probation window. */
static void test_slot_probation_requires_confirmation(void)
{
    zv_ota_slot_t slot;
    zv_ota_slot_init(&slot);
    ZV_CHECK_EQ(zv_ota_slot_begin(&slot, IMAGE_SIZE), ZV_OK);
    ZV_CHECK(!zv_ota_slot_probation_expired(&slot, 999999u, ZV_OTA_PROBATION_MS_DEFAULT));
    ZV_CHECK_EQ(zv_ota_slot_begin_probation(&slot, 0u), ZV_ERR_INVALID_STATE);
    ZV_CHECK_EQ(zv_ota_slot_begin_probation(NULL, 0u), ZV_ERR_INVALID_ARG);
}

/**
 * Sequenced transfers: a gap abandons the update rather than writing a hole
 * into the image, and a transfer that stops delivering data is abandoned
 * instead of leaving the slot pending.
 */
static void test_slot_sequence_and_stall(void)
{
    zv_ota_slot_t slot;
    zv_ota_slot_init(&slot);
    ZV_CHECK_EQ(zv_ota_slot_begin(&slot, IMAGE_SIZE), ZV_OK);

    /* Consecutive sequence numbers are accepted. */
    ZV_CHECK_EQ(zv_ota_slot_write_seq(&slot, 0u, 100u, 1000u, ZV_OTA_STALL_TIMEOUT_MS_DEFAULT), ZV_OK);
    ZV_CHECK_EQ(zv_ota_slot_write_seq(&slot, 1u, 100u, 1100u, ZV_OTA_STALL_TIMEOUT_MS_DEFAULT), ZV_OK);
    ZV_CHECK_EQ(slot.received, 200u);

    /* A gap abandons the update. */
    ZV_CHECK_EQ(zv_ota_slot_write_seq(&slot, 5u, 100u, 1200u, ZV_OTA_STALL_TIMEOUT_MS_DEFAULT),
                ZV_ERR_POLICY);
    ZV_CHECK_EQ(slot.state, ZV_OTA_SLOT_FAILED);
    ZV_CHECK_EQ(slot.reason, ZV_OTA_REASON_SEQUENCE_GAP);
    ZV_CHECK_STR(zv_ota_reason_name(slot.reason), "sequence_gap");

    /* A stalled transfer is abandoned as well. */
    zv_ota_slot_init(&slot);
    ZV_CHECK_EQ(zv_ota_slot_begin(&slot, IMAGE_SIZE), ZV_OK);
    ZV_CHECK_EQ(zv_ota_slot_write_seq(&slot, 0u, 100u, 1000u, ZV_OTA_STALL_TIMEOUT_MS_DEFAULT), ZV_OK);
    ZV_CHECK(!zv_ota_slot_stalled(&slot, 1000u, ZV_OTA_STALL_TIMEOUT_MS_DEFAULT));
    ZV_CHECK(zv_ota_slot_stalled(&slot, 1000u + ZV_OTA_STALL_TIMEOUT_MS_DEFAULT,
                                 ZV_OTA_STALL_TIMEOUT_MS_DEFAULT));
    ZV_CHECK_EQ(zv_ota_slot_write_seq(&slot, 1u, 100u, 1000u + ZV_OTA_STALL_TIMEOUT_MS_DEFAULT,
                                      ZV_OTA_STALL_TIMEOUT_MS_DEFAULT), ZV_ERR_POLICY);
    ZV_CHECK_EQ(slot.reason, ZV_OTA_REASON_STALLED);
    ZV_CHECK_STR(zv_ota_reason_name(slot.reason), "stalled");

    /* A slot that is not downloading cannot stall. */
    ZV_CHECK(!zv_ota_slot_stalled(&slot, 1000u + ZV_OTA_STALL_TIMEOUT_MS_DEFAULT * 2u,
                                  ZV_OTA_STALL_TIMEOUT_MS_DEFAULT));
}

/* Failed transfers can restart on the same slot without a full init. */
static void test_slot_sequence_retry_resets_bookkeeping(void)
{
    zv_ota_slot_t slot;
    zv_ota_slot_init(&slot);
    ZV_CHECK_EQ(zv_ota_slot_begin(&slot, 200u), ZV_OK);
    ZV_CHECK_EQ(zv_ota_slot_write_seq(&slot, 0u, 100u, 1000u, 30000u), ZV_OK);
    ZV_CHECK_EQ(zv_ota_slot_write_seq(&slot, 2u, 100u, 1100u, 30000u), ZV_ERR_POLICY);
    ZV_CHECK_EQ(slot.reason, ZV_OTA_REASON_SEQUENCE_GAP);

    ZV_CHECK_EQ(zv_ota_slot_begin(&slot, 200u), ZV_OK);
    ZV_CHECK(!slot.sequence_started && slot.next_sequence == 0u);
    ZV_CHECK_EQ(slot.last_activity_ms, 0u);
    ZV_CHECK_EQ(slot.received, 0u);
    ZV_CHECK_EQ(slot.reason, ZV_OTA_REASON_NONE);
    ZV_CHECK(!zv_ota_slot_stalled(&slot, 90000u, 30000u));
    ZV_CHECK_EQ(zv_ota_slot_write_seq(&slot, 0u, 100u, 90000u, 30000u), ZV_OK);
    ZV_CHECK_EQ(zv_ota_slot_write_seq(&slot, 1u, 100u, 120000u, 30000u), ZV_ERR_POLICY);
    ZV_CHECK_EQ(slot.reason, ZV_OTA_REASON_STALLED);

    ZV_CHECK_EQ(zv_ota_slot_begin(&slot, 200u), ZV_OK);
    ZV_CHECK_EQ(zv_ota_slot_write_seq(&slot, 0u, 100u, 150000u, 30000u), ZV_OK);
    ZV_CHECK_EQ(zv_ota_slot_write_seq(&slot, 1u, 100u, 150100u, 30000u), ZV_OK);
    ZV_CHECK_EQ(zv_ota_slot_finish_download(&slot), ZV_OK);
    ZV_CHECK_EQ(zv_ota_slot_digest_result(&slot, true), ZV_OK);
    ZV_CHECK_EQ(zv_ota_slot_begin_probation(&slot, 151000u), ZV_OK);
    ZV_CHECK_EQ(slot.probation_started_ms, 151000u);
}


/* A zero timestamp is a valid start, not an unstarted sentinel. */
static void test_slot_probation_entry_and_zero_timestamp(void)
{
    uint8_t digest[32];
    fill_digest(digest, 6u);
    zv_ota_slot_t slot;
    zv_ota_slot_init(&slot);
    ZV_CHECK_EQ(drive_to_awaiting(&slot, digest), ZV_OK);
    ZV_CHECK(!zv_ota_slot_probation_expired(&slot, 999999u, 300000u));
    ZV_CHECK_EQ(zv_ota_slot_begin_probation(&slot, 0u), ZV_OK);
    ZV_CHECK(!zv_ota_slot_probation_expired(&slot, 299999u, 300000u));
    ZV_CHECK(zv_ota_slot_probation_expired(&slot, 300000u, 300000u));
    ZV_CHECK(!zv_ota_slot_probation_expired(&slot, 300000u, 0u));
    ZV_CHECK_EQ(zv_ota_slot_boot_ok(&slot), ZV_OK);
    ZV_CHECK(!zv_ota_slot_probation_expired(&slot, 999999u, 300000u));

    /* Reset and retry clear the entry flag; wraparound preserves the window. */
    zv_ota_slot_init(&slot);
    ZV_CHECK_EQ(drive_to_awaiting(&slot, digest), ZV_OK);
    ZV_CHECK(!zv_ota_slot_probation_expired(&slot, 999999u, 300000u));
    ZV_CHECK_EQ(zv_ota_slot_begin_probation(&slot, UINT32_MAX - 10u), ZV_OK);
    ZV_CHECK(!zv_ota_slot_probation_expired(&slot, 8u, 20u));
    ZV_CHECK(zv_ota_slot_probation_expired(&slot, 9u, 20u));
    ZV_CHECK_EQ(zv_ota_slot_boot_failed(&slot), ZV_OK);
    ZV_CHECK(!zv_ota_slot_probation_expired(&slot, 10u, 20u));

    zv_ota_slot_init(&slot);
    ZV_CHECK_EQ(zv_ota_slot_begin(&slot, IMAGE_SIZE), ZV_OK);
    ZV_CHECK_EQ(zv_ota_slot_finish_download(&slot), ZV_ERR_POLICY);
    ZV_CHECK_EQ(drive_to_awaiting(&slot, digest), ZV_OK);
    ZV_CHECK(!zv_ota_slot_probation_expired(&slot, 999999u, 300000u));
}

void test_suite_ota_slot(void)
{
    test_digest_equal();
    test_slot_happy_path();
    test_slot_rejects_illegal_shortcuts();
    test_slot_bounds_writes();
    test_slot_digest_mismatch_is_not_confirmable();
    test_slot_rolls_back_failed_self_test();
    test_slot_retry_after_failure();
    test_slot_state_names();
    test_slot_follows_manifest_policy();
    test_slot_probation_window();
    test_slot_probation_entry_and_zero_timestamp();
    test_slot_probation_requires_confirmation();
    test_slot_sequence_and_stall();
    test_slot_sequence_retry_resets_bookkeeping();
}
