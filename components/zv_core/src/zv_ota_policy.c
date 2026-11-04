/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 */
#include "zv_ota_policy.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

static bool parse_component(const char **cursor, char terminator, uint16_t *out)
{
    const char *p = *cursor;
    uint32_t value = 0u;
    size_t digits = 0u;
    while (*p >= '0' && *p <= '9') {
        if (digits == 5u) {
            return false;
        }
        value = value * 10u + (uint32_t)(*p - '0');
        digits++;
        p++;
    }
    if (digits == 0u || value > 65535u || (digits > 1u && **cursor == '0') || *p != terminator) {
        return false;
    }
    *out = (uint16_t)value;
    *cursor = (terminator == '\0') ? p : p + 1;
    return true;
}

zv_status_t zv_semver_parse(const char *text, zv_semver_t *out)
{
    if (text == NULL || out == NULL) {
        return ZV_ERR_INVALID_ARG;
    }
    const char *cursor = text;
    zv_semver_t parsed = {0u, 0u, 0u};
    if (!parse_component(&cursor, '.', &parsed.major) || !parse_component(&cursor, '.', &parsed.minor) ||
        !parse_component(&cursor, '\0', &parsed.patch)) {
        return ZV_ERR_INVALID_ARG;
    }
    *out = parsed;
    return ZV_OK;
}

int zv_semver_compare(const zv_semver_t *a, const zv_semver_t *b)
{
    if (a->major != b->major) {
        return a->major < b->major ? -1 : 1;
    }
    if (a->minor != b->minor) {
        return a->minor < b->minor ? -1 : 1;
    }
    if (a->patch != b->patch) {
        return a->patch < b->patch ? -1 : 1;
    }
    return 0;
}

static bool valid_sha256_hex(const char *s)
{
    for (size_t i = 0u; i < ZV_OTA_SHA256_LEN; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return s[ZV_OTA_SHA256_LEN] == '\0';
}

static zv_status_t reject(zv_ota_reason_t *reason, zv_ota_reason_t value)
{
    *reason = value;
    return ZV_ERR_POLICY;
}

zv_status_t zv_ota_evaluate(const zv_ota_manifest_t *manifest, const zv_ota_device_t *device,
                            zv_ota_reason_t *reason)
{
    if (manifest == NULL || device == NULL || reason == NULL || device->running_version == NULL ||
        device->board_id == NULL || device->channel == NULL) {
        return ZV_ERR_INVALID_ARG;
    }
    *reason = ZV_OTA_REASON_NONE;

    zv_semver_t running = {0u, 0u, 0u};
    if (zv_semver_parse(device->running_version, &running) != ZV_OK) {
        return ZV_ERR_INVALID_ARG;
    }
    zv_semver_t candidate = {0u, 0u, 0u};
    if (zv_semver_parse(manifest->version, &candidate) != ZV_OK || manifest->board_id[0] == '\0' ||
        manifest->channel[0] == '\0' || !valid_sha256_hex(manifest->sha256)) {
        return reject(reason, ZV_OTA_REASON_BAD_MANIFEST);
    }
    if (strcmp(manifest->board_id, device->board_id) != 0) {
        return reject(reason, ZV_OTA_REASON_BOARD_MISMATCH);
    }
    if (strcmp(manifest->channel, device->channel) != 0) {
        return reject(reason, ZV_OTA_REASON_CHANNEL_MISMATCH);
    }
    if (strcmp(manifest->signature_scheme, ZV_OTA_SIG_SCHEME_SECURE_BOOT_V2) != 0) {
        return reject(reason, ZV_OTA_REASON_UNSIGNED);
    }
    if (manifest->secure_version < device->efuse_secure_version) {
        return reject(reason, ZV_OTA_REASON_SECURE_VERSION_ROLLBACK);
    }
    if (manifest->image_size == 0u || manifest->image_size > device->partition_size) {
        return reject(reason, ZV_OTA_REASON_TOO_LARGE);
    }
    if (zv_semver_compare(&candidate, &running) <= 0) {
        return reject(reason, ZV_OTA_REASON_NOT_NEWER);
    }
    return ZV_OK;
}

const char *zv_ota_reason_name(zv_ota_reason_t reason)
{
    switch (reason) {
    case ZV_OTA_REASON_NONE:
        return "none";
    case ZV_OTA_REASON_BAD_MANIFEST:
        return "bad_manifest";
    case ZV_OTA_REASON_BOARD_MISMATCH:
        return "board_mismatch";
    case ZV_OTA_REASON_CHANNEL_MISMATCH:
        return "channel_mismatch";
    case ZV_OTA_REASON_NOT_NEWER:
        return "not_newer";
    case ZV_OTA_REASON_SECURE_VERSION_ROLLBACK:
        return "secure_version_rollback";
    case ZV_OTA_REASON_TOO_LARGE:
        return "too_large";
    case ZV_OTA_REASON_UNSIGNED:
        return "unsigned";
    case ZV_OTA_REASON_DIGEST_MISMATCH:
        return "digest_mismatch";
    }
    return "unknown";
}

bool zv_ota_digest_equal(const char *expected_hex, const uint8_t *digest, size_t digest_len)
{
    if (expected_hex == NULL || digest == NULL || digest_len != 32u) {
        return false;
    }
    if (!valid_sha256_hex(expected_hex)) {
        return false;
    }
    /* Compare case-sensitively against lower-case hex; do not normalise input. */
    static const char k_hex[] = "0123456789abcdef";
    for (size_t i = 0u; i < 32u; i++) {
        uint8_t byte = digest[i];
        char high = k_hex[(byte >> 4) & 0x0Fu];
        char low = k_hex[byte & 0x0Fu];
        if (expected_hex[i * 2u] != high || expected_hex[(i * 2u) + 1u] != low) {
            return false;
        }
    }
    return true;
}

void zv_ota_slot_init(zv_ota_slot_t *slot)
{
    if (slot != NULL) {
        memset(slot, 0, sizeof(*slot));
        slot->state = ZV_OTA_SLOT_IDLE;
        slot->reason = ZV_OTA_REASON_NONE;
    }
}

static zv_status_t slot_fail(zv_ota_slot_t *slot, zv_ota_reason_t reason)
{
    slot->state = ZV_OTA_SLOT_FAILED;
    slot->reason = reason;
    return ZV_ERR_POLICY;
}

zv_status_t zv_ota_slot_begin(zv_ota_slot_t *slot, uint32_t expected_size)
{
    if (slot == NULL || expected_size == 0u) {
        return ZV_ERR_INVALID_ARG;
    }
    if (slot->state != ZV_OTA_SLOT_IDLE && slot->state != ZV_OTA_SLOT_FAILED) {
        return ZV_ERR_INVALID_STATE;
    }
    slot->state = ZV_OTA_SLOT_DOWNLOADING;
    slot->expected_size = expected_size;
    slot->received = 0u;
    slot->reason = ZV_OTA_REASON_NONE;
    return ZV_OK;
}

zv_status_t zv_ota_slot_write(zv_ota_slot_t *slot, uint32_t chunk_len)
{
    if (slot == NULL) {
        return ZV_ERR_INVALID_ARG;
    }
    if (slot->state != ZV_OTA_SLOT_DOWNLOADING) {
        return ZV_ERR_INVALID_STATE;
    }
    if (chunk_len == 0u || chunk_len > ZV_OTA_SLOT_CHUNK_MAX) {
        return ZV_ERR_INVALID_ARG;
    }
    /* Subtraction rather than addition so the bound cannot wrap. */
    if (chunk_len > slot->expected_size - slot->received) {
        return slot_fail(slot, ZV_OTA_REASON_TOO_LARGE);
    }
    slot->received += chunk_len;
    return ZV_OK;
}

zv_status_t zv_ota_slot_finish_download(zv_ota_slot_t *slot)
{
    if (slot == NULL) {
        return ZV_ERR_INVALID_ARG;
    }
    if (slot->state != ZV_OTA_SLOT_DOWNLOADING) {
        return ZV_ERR_INVALID_STATE;
    }
    /* A short transfer must never reach the digest stage. */
    if (slot->received != slot->expected_size) {
        return slot_fail(slot, ZV_OTA_REASON_BAD_MANIFEST);
    }
    slot->state = ZV_OTA_SLOT_VERIFYING;
    return ZV_OK;
}

zv_status_t zv_ota_slot_digest_result(zv_ota_slot_t *slot, bool match)
{
    if (slot == NULL) {
        return ZV_ERR_INVALID_ARG;
    }
    if (slot->state != ZV_OTA_SLOT_VERIFYING) {
        return ZV_ERR_INVALID_STATE;
    }
    if (!match) {
        return slot_fail(slot, ZV_OTA_REASON_DIGEST_MISMATCH);
    }
    slot->state = ZV_OTA_SLOT_AWAITING_CONFIRM;
    slot->reason = ZV_OTA_REASON_NONE;
    return ZV_OK;
}

zv_status_t zv_ota_slot_boot_ok(zv_ota_slot_t *slot)
{
    if (slot == NULL) {
        return ZV_ERR_INVALID_ARG;
    }
    if (slot->state != ZV_OTA_SLOT_AWAITING_CONFIRM) {
        return ZV_ERR_INVALID_STATE;
    }
    slot->state = ZV_OTA_SLOT_CONFIRMED;
    return ZV_OK;
}

zv_status_t zv_ota_slot_boot_failed(zv_ota_slot_t *slot)
{
    if (slot == NULL) {
        return ZV_ERR_INVALID_ARG;
    }
    if (slot->state != ZV_OTA_SLOT_AWAITING_CONFIRM) {
        return ZV_ERR_INVALID_STATE;
    }
    slot->state = ZV_OTA_SLOT_ROLLED_BACK;
    return ZV_OK;
}

const char *zv_ota_slot_state_name(zv_ota_slot_state_t state)
{
    switch (state) {
    case ZV_OTA_SLOT_IDLE:
        return "idle";
    case ZV_OTA_SLOT_DOWNLOADING:
        return "downloading";
    case ZV_OTA_SLOT_VERIFYING:
        return "verifying";
    case ZV_OTA_SLOT_AWAITING_CONFIRM:
        return "awaiting_confirm";
    case ZV_OTA_SLOT_CONFIRMED:
        return "confirmed";
    case ZV_OTA_SLOT_ROLLED_BACK:
        return "rolled_back";
    case ZV_OTA_SLOT_FAILED:
        return "failed";
    }
    return "unknown";
}
