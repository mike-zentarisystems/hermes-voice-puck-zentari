/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "zv_status.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t major;
    uint16_t minor;
    uint16_t patch;
} zv_semver_t;

/** Strict MAJOR.MINOR.PATCH, each 0..65535, no prefix or pre-release suffix. */
zv_status_t zv_semver_parse(const char *text, zv_semver_t *out);
int zv_semver_compare(const zv_semver_t *a, const zv_semver_t *b);

#define ZV_OTA_VERSION_MAX 23u
#define ZV_OTA_BOARD_ID_MAX 63u
#define ZV_OTA_CHANNEL_MAX 15u
#define ZV_OTA_SHA256_LEN 64u
#define ZV_OTA_SIG_SCHEME_MAX 31u
#define ZV_OTA_SIG_SCHEME_SECURE_BOOT_V2 "esp-secure-boot-v2-rsa3072"

/** Parsed ota.available payload. */
typedef struct {
    char version[ZV_OTA_VERSION_MAX + 1u];
    char board_id[ZV_OTA_BOARD_ID_MAX + 1u];
    char channel[ZV_OTA_CHANNEL_MAX + 1u];
    char sha256[ZV_OTA_SHA256_LEN + 1u];
    char signature_scheme[ZV_OTA_SIG_SCHEME_MAX + 1u];
    uint32_t secure_version;
    uint32_t image_size;
} zv_ota_manifest_t;

typedef struct {
    const char *running_version;
    const char *board_id;
    const char *channel;
    uint32_t efuse_secure_version; /**< Anti-rollback floor burned in eFuse */
    uint32_t partition_size;       /**< Size of the next OTA slot */
} zv_ota_device_t;

typedef enum {
    ZV_OTA_REASON_NONE = 0,
    ZV_OTA_REASON_BAD_MANIFEST,
    ZV_OTA_REASON_BOARD_MISMATCH,
    ZV_OTA_REASON_CHANNEL_MISMATCH,
    ZV_OTA_REASON_NOT_NEWER,
    ZV_OTA_REASON_SECURE_VERSION_ROLLBACK,
    ZV_OTA_REASON_TOO_LARGE,
    ZV_OTA_REASON_UNSIGNED,
    ZV_OTA_REASON_DIGEST_MISMATCH,
} zv_ota_reason_t;

/**
 * Pre-download gate. ZV_OK means download may start; the image is still
 * verified after download (SHA-256 + Secure Boot V2 signature) before
 * esp_ota_set_boot_partition(). Any rejection returns ZV_ERR_POLICY and a reason.
 */
zv_status_t zv_ota_evaluate(const zv_ota_manifest_t *manifest, const zv_ota_device_t *device,
                            zv_ota_reason_t *reason);
const char *zv_ota_reason_name(zv_ota_reason_t reason);

/**
 * Compares a raw SHA-256 digest against the manifest's lower-case hex digest.
 * A mismatch means a corrupt or substituted image, so the caller must not mark
 * the slot bootable. zv_core stays free of a crypto dependency; the hash itself
 * is computed by the platform layer.
 */
bool zv_ota_digest_equal(const char *expected_hex, const uint8_t *digest, size_t digest_len);

/**
 * A/B slot lifecycle, mirroring the ESP-IDF otadata states the bootloader uses.
 *
 * The ordering is the safety property: an image cannot reach CONFIRMED without
 * a complete download and a digest match first, and a failed boot self-test
 * rolls the slot back rather than leaving it pending forever.
 */
typedef enum {
    ZV_OTA_SLOT_IDLE = 0,           /**< no update in progress */
    ZV_OTA_SLOT_DOWNLOADING,        /**< accepting image chunks */
    ZV_OTA_SLOT_VERIFYING,          /**< download complete, digest in flight */
    ZV_OTA_SLOT_AWAITING_CONFIRM,   /**< verified and marked valid, untested boot */
    ZV_OTA_SLOT_CONFIRMED,          /**< booted and passed self-test */
    ZV_OTA_SLOT_ROLLED_BACK,        /**< booted but failed self-test */
    ZV_OTA_SLOT_FAILED,             /**< rejected; reason records why */
} zv_ota_slot_state_t;

/** Largest accepted write. Bounds a single stack-resident caller buffer. */
#define ZV_OTA_SLOT_CHUNK_MAX 4096u

typedef struct {
    zv_ota_slot_state_t state;
    uint32_t expected_size; /**< from the accepted manifest */
    uint32_t received;      /**< bytes accepted so far, never exceeds expected_size */
    zv_ota_reason_t reason; /**< set when state becomes FAILED */
} zv_ota_slot_t;

void zv_ota_slot_init(zv_ota_slot_t *slot);

/** Starts an update. Only legal from IDLE or FAILED; expected_size must be > 0. */
zv_status_t zv_ota_slot_begin(zv_ota_slot_t *slot, uint32_t expected_size);

/**
 * Accounts one received chunk. Rejects an empty chunk, an oversized chunk, and
 * any write that would push the total past the accepted manifest size, so a
 * hostile or truncated stream cannot fill the inactive slot.
 */
zv_status_t zv_ota_slot_write(zv_ota_slot_t *slot, uint32_t chunk_len);

/** Requires the full expected size before the digest is considered. */
zv_status_t zv_ota_slot_finish_download(zv_ota_slot_t *slot);

/** match=false moves to FAILED with ZV_OTA_REASON_DIGEST_MISMATCH. */
zv_status_t zv_ota_slot_digest_result(zv_ota_slot_t *slot, bool match);

/** Self-test passed: the new image is now the confirmed one. */
zv_status_t zv_ota_slot_boot_ok(zv_ota_slot_t *slot);

/** Self-test failed: roll back to the previous slot. */
zv_status_t zv_ota_slot_boot_failed(zv_ota_slot_t *slot);

const char *zv_ota_slot_state_name(zv_ota_slot_state_t state);

#ifdef __cplusplus
}
#endif
