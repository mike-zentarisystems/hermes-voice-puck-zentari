/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 *
 * M4 signed A/B OTA.
 *
 * This deliberately uses the raw esp_ota_begin/esp_ota_write/esp_ota_end
 * partition API plus esp_http_client rather than esp_https_ota.
 *
 * esp_https_ota_finish() calls esp_ota_set_boot_partition() internally, so the
 * one-shot helper stages the new image the moment the download completes. That
 * is too early here: this project requires that the SHA-256 of what actually
 * landed in flash matches the manifest BEFORE the bootloader is pointed at it.
 * Doing the transfer by hand is what keeps that ordering enforceable.
 */
#include "zv_ota.h"

#include <string.h>

#include "sdkconfig.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "psa/crypto.h"
#include "zv_identity.h"

static const char *TAG = "zv_ota";

esp_err_t zv_ota_hash_partition(const char *label, uint32_t image_size, const char *expected_sha256_hex,
                                bool *match)
{
    if (match == NULL || expected_sha256_hex == NULL || image_size == 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    *match = false;

    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (target == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (image_size > target->size) {
        ESP_LOGE(TAG, "%s: image %u exceeds the %u byte slot", label, (unsigned)image_size,
                 (unsigned)target->size);
        return ESP_ERR_INVALID_SIZE;
    }

    /* Hashed on the stack-sized chunk buffer; no large allocation on the task. */
    uint8_t chunk[ZV_OTA_READ_CHUNK];
    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
    psa_status_t status = psa_hash_setup(&op, PSA_ALG_SHA_256);
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "%s: SHA-256 setup failed: %d", label, (int)status);
        return ESP_FAIL;
    }

    uint32_t offset = 0u;
    while (status == PSA_SUCCESS && offset < image_size) {
        uint32_t want = image_size - offset;
        if (want > ZV_OTA_READ_CHUNK) {
            want = ZV_OTA_READ_CHUNK;
        }
        esp_err_t err = esp_partition_read(target, offset, chunk, want);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "%s: partition read failed at %u: %s", label, (unsigned)offset, esp_err_to_name(err));
            psa_hash_abort(&op);
            return err;
        }
        status = psa_hash_update(&op, chunk, want);
        offset += want;
        /* Keep the operator and the watchdog informed during a long read-back. */
        taskYIELD();
    }

    uint8_t digest[32];
    size_t digest_len = sizeof(digest);
    if (status == PSA_SUCCESS) {
        status = psa_hash_finish(&op, digest, sizeof(digest), &digest_len);
    } else {
        psa_hash_abort(&op);
    }
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "%s: SHA-256 failed: %d", label, (int)status);
        return ESP_FAIL;
    }

    *match = zv_ota_digest_equal(expected_sha256_hex, digest, digest_len);
    memset(digest, 0, sizeof(digest));
    ESP_LOGI(TAG, "%s: digest match=%d (read %u bytes)", label, (int)*match, (unsigned)image_size);
    return ESP_OK;
}

esp_err_t zv_ota_apply(const zv_ota_manifest_t *manifest, const zv_ota_device_t *device, const char *url,
                       zv_ota_result_t *out)
{
    if (manifest == NULL || device == NULL || url == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    out->slot_state = ZV_OTA_SLOT_IDLE;
    out->reason = ZV_OTA_REASON_NONE;

    /* 1. Pre-download policy. Nothing is fetched if this rejects. */
    zv_ota_reason_t reason = ZV_OTA_REASON_NONE;
    if (zv_ota_evaluate(manifest, device, &reason) != ZV_OK) {
        out->reason = reason;
        out->slot_state = ZV_OTA_SLOT_FAILED;
        ESP_LOGE(TAG, "OTA refused before download: %s", zv_ota_reason_name(reason));
        return ESP_ERR_INVALID_STATE;
    }

    /* esp_ota_end only checks signatures when the application's update
     * verifier is enabled. Our manifest requires the RSA-3072 V2 scheme.
     * A lab/default build must refuse signed OTA before network or flash I/O. */
#if !CONFIG_SECURE_SIGNED_ON_UPDATE || !CONFIG_SECURE_SIGNED_APPS_RSA_SCHEME
    out->reason = ZV_OTA_REASON_UNSIGNED;
    out->slot_state = ZV_OTA_SLOT_FAILED;
    ESP_LOGE(TAG, "OTA requires RSA-3072 signature verification on update");
    return ESP_ERR_INVALID_STATE;
#endif

    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (target == NULL) {
        ESP_LOGE(TAG, "no inactive OTA partition available");
        return ESP_ERR_NOT_FOUND;
    }

    /* 2. TLS with the certificate bundle and hostname verification both on. */
    esp_http_client_config_t http_cfg = {
        .url = url,
        .timeout_ms = 120000,
        .keep_alive_enable = true,
        .skip_cert_common_name_check = false,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t http = esp_http_client_init(&http_cfg);
    if (http == NULL) {
        ESP_LOGE(TAG, "esp_http_client_init failed");
        return ESP_FAIL;
    }

    esp_err_t err = ESP_OK;
    esp_ota_handle_t ota = 0;
    bool ota_open = false;
    zv_ota_slot_t slot;
    zv_ota_slot_init(&slot);

    do {
        esp_http_client_set_method(http, HTTP_METHOD_GET);
        err = esp_http_client_open(http, 0);
        if (err != ESP_OK) {
            break;
        }
        int64_t announced = esp_http_client_fetch_headers(http);
        if (announced < 0) {
            err = ESP_FAIL;
            break;
        }

        int status_code = esp_http_client_get_status_code(http);
        if (status_code != 200) {
            ESP_LOGE(TAG, "OTA server returned HTTP %d", status_code);
            err = ESP_FAIL;
            break;
        }

        int64_t content_length = esp_http_client_get_content_length(http);
        if (content_length > 0) {
            if (content_length > (int64_t)manifest->image_size) {
                ESP_LOGE(TAG, "announced length %lld exceeds the accepted %u byte image",
                         (long long)content_length, (unsigned)manifest->image_size);
                err = ESP_ERR_INVALID_SIZE;
                break;
            }
            out->image_size = (uint32_t)content_length;
        } else {
            /* Unknown length: fall back to the manifest budget as the hard cap. */
            out->image_size = manifest->image_size;
        }

        err = zv_ota_slot_begin(&slot, manifest->image_size);
        if (err != ZV_OK) {
            err = ESP_ERR_INVALID_STATE;
            break;
        }

        /* Sequential writes keep the erase pattern simple and predictable. */
        err = esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &ota);
        if (err != ESP_OK) {
            break;
        }
        ota_open = true;

        uint8_t buffer[ZV_OTA_READ_CHUNK];
        while (true) {
            int read_len = esp_http_client_read(http, (char *)buffer, sizeof(buffer));
            if (read_len < 0) {
                err = ESP_FAIL;
                break;
            }
            if (read_len == 0) {
                break; /* end of stream */
            }
            if (zv_ota_slot_write(&slot, (uint32_t)read_len) != ZV_OK) {
                ESP_LOGE(TAG, "OTA stream overran the accepted image size");
                err = ESP_ERR_INVALID_SIZE;
                break;
            }
            err = esp_ota_write(ota, buffer, (uint32_t)read_len);
            if (err != ESP_OK) {
                break;
            }
        }
        if (err != ESP_OK) {
            break;
        }

        out->downloaded = slot.received;
        if (zv_ota_slot_finish_download(&slot) != ZV_OK) {
            ESP_LOGE(TAG, "OTA download ended short of the accepted size");
            err = ESP_ERR_INVALID_SIZE;
            break;
        }

        /* 3. esp_ota_end validates the image and runs the Secure Boot V2
         * RSA-3072 signature check required by the pre-download config gate. It does NOT change the
         * boot partition, which is the whole reason for this code path. */
        err = esp_ota_end(ota);
        ota_open = false;
        if (err != ESP_OK) break;
        uint32_t written = slot.received; /* esp_ota_end returns status, not bytes. */
        out->download_complete = true;
        out->signature_verified = true;
        out->image_size = (uint32_t)written;

        /* 4. Re-hash what actually landed in flash and compare. */
        bool digest_match = false;
        err = zv_ota_hash_partition("ota", (uint32_t)written, manifest->sha256, &digest_match);
        if (err != ESP_OK) {
            break;
        }
        out->digest_match = digest_match;
        if (zv_ota_slot_digest_result(&slot, digest_match) != ZV_OK) {
            ESP_LOGE(TAG, "OTA image digest did not match the manifest; boot partition left unchanged");
            err = ESP_ERR_INVALID_STATE;
            break;
        }

        /* 5. Only now may the bootloader be pointed at the new image. */
        err = esp_ota_set_boot_partition(target);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
            break;
        }
        out->boot_partition_set = true;
        out->image_fits_slot = true;
    } while (false);

    esp_http_client_cleanup(http);
    if (ota_open) {
        /* Marks the slot abortable; the running partition is untouched. */
        esp_ota_abort(ota);
    }

    out->slot_state = slot.state;
    if (slot.state == ZV_OTA_SLOT_FAILED) {
        out->reason = slot.reason;
    }

    if (err == ESP_OK) {
        ESP_LOGI(TAG,
                 "OTA staged: version=%s size=%u digest_ok=%d boot_partition_set=1 slot=%s; "
                 "reboot is an explicit operator action",
                 manifest->version, (unsigned)out->image_size, (int)out->digest_match,
                 zv_ota_slot_state_name(slot.state));
    } else {
        ESP_LOGE(TAG, "OTA failed: %s slot=%s reason=%s", esp_err_to_name(err),
                 zv_ota_slot_state_name(slot.state), zv_ota_reason_name(out->reason));
    }
    return err;
}
