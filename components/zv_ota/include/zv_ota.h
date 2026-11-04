/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 *
 * M4 signed A/B OTA: raw esp_ota download driven by the portable
 * zv_ota_slot_t state machine.
 *
 * The ordering is the safety property and it lives in zv_core, not here:
 * a complete download and a matching SHA-256 must both happen before the
 * bootloader is told to boot the new image.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "zv_ota_policy.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Chunk size for reading the written image back for hashing. */
#define ZV_OTA_READ_CHUNK 1024u

typedef struct {
    uint32_t image_size;        /**< bytes the server announced */
    uint32_t downloaded;        /**< bytes accepted by the slot accountant */
    bool image_fits_slot;       /**< announced size is inside the manifest budget */
    bool download_complete;     /**< the full manifest size was received */
    bool signature_verified;    /**< esp_ota_end verified the RSA-3072 signature */
    bool digest_match;          /**< recomputed SHA-256 equals the manifest digest */
    bool boot_partition_set;    /**< esp_ota_set_boot_partition was called */
    zv_ota_slot_state_t slot_state;
    zv_ota_reason_t reason;     /**< populated when slot_state is FAILED */
} zv_ota_result_t;

/**
 * Runs one OTA update end to end and does not reboot.
 *
 * Order of operations:
 *   1. zv_ota_evaluate applies the pre-download board/channel/version/secure-version
 *      and size policy. RSA-3072 signature verification on update must also be
 *      enabled in sdkconfig. A rejection stops before network or flash I/O.
 *   2. esp_http_client opens the URL with TLS and hostname verification.
 *   3. The download is accounted through zv_ota_slot_write, so a stream that
 *      overruns the accepted manifest size fails the slot.
 *   4. zv_ota_slot_finish_download requires the full manifest size; esp_ota_end must accept
 *      the image and its signature.
 *   5. The image is read back from the inactive partition and SHA-256'd, then
 *      compared with zv_ota_digest_equal against the manifest digest.
 *   6. esp_ota_set_boot_partition is called only when the digest matched.
 *
 * Returns ESP_OK only when the image is downloaded, verified, and staged to
 * boot. Every other outcome returns an error and leaves the running image as
 * the boot target.
 */
esp_err_t zv_ota_apply(const zv_ota_manifest_t *manifest, const zv_ota_device_t *device, const char *url,
                       zv_ota_result_t *out);

/**
 * Reads back an OTA partition and reports whether its SHA-256 matches
 * `expected_sha256_hex`. Exposed so the digest stage can be exercised
 * independently of a network download.
 */
esp_err_t zv_ota_hash_partition(const char *label, uint32_t image_size, const char *expected_sha256_hex,
                                bool *match);

#ifdef __cplusplus
}
#endif