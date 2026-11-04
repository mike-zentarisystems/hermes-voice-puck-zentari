/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 */
#include "zv_storage.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "zv_storage";

#define NVS_NS "zv_cred"
#define KEY_ACTIVE_META "a_meta"
#define KEY_ACTIVE_SECRET "a_sec"
#define KEY_STAGED_META "s_meta"
#define KEY_STAGED_SECRET "s_sec"

static bool s_ready;

/** Meta is everything except the secret bytes. */
typedef struct {
    char device_id[ZV_CRED_DEVICE_ID_MAX + 1u];
    char gateway_origin[ZV_CRED_ORIGIN_MAX + 1u];
    uint32_t scope_mask;
    uint32_t generation;
    int64_t expires_at_s;
    uint32_t secret_len;
} cred_meta_t;

esp_err_t zv_storage_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    const esp_partition_t *keys = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS_KEYS, NULL);
    ESP_RETURN_ON_FALSE(keys != NULL, ESP_ERR_NOT_FOUND, TAG, "nvs_keys partition missing");

    nvs_sec_cfg_t cfg = {};
    esp_err_t err = nvs_flash_read_security_cfg(keys, &cfg);
    if (err == ESP_ERR_NVS_KEYS_NOT_INITIALIZED) {
        ESP_LOGW(TAG, "generating NVS encryption keys in nvs_keys (lab; production uses pre-provisioned keys)");
        err = nvs_flash_generate_keys(keys, &cfg);
    }
    ESP_RETURN_ON_ERROR(err, TAG, "NVS security config unavailable");

    err = nvs_flash_secure_init_partition(NVS_DEFAULT_PART_NAME, &cfg);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "erasing NVS partition after init error %s", esp_err_to_name(err));
        ESP_RETURN_ON_ERROR(nvs_flash_erase_partition(NVS_DEFAULT_PART_NAME), TAG, "NVS erase failed");
        err = nvs_flash_secure_init_partition(NVS_DEFAULT_PART_NAME, &cfg);
    }
    ESP_RETURN_ON_ERROR(err, TAG, "encrypted NVS init failed");

    s_ready = true;
    ESP_LOGI(TAG, "encrypted NVS ready (nvs_keys present; flash encryption / Secure Boot still required for production)");
    return ESP_OK;
}

static esp_err_t write_cred(nvs_handle_t h, const char *meta_key, const char *sec_key,
                            const zv_device_credential_t *cred, bool present)
{
    if (!present) {
        (void)nvs_erase_key(h, meta_key);
        (void)nvs_erase_key(h, sec_key);
        return ESP_OK;
    }
    cred_meta_t meta = {0};
    memcpy(meta.device_id, cred->device_id, sizeof(meta.device_id));
    memcpy(meta.gateway_origin, cred->gateway_origin, sizeof(meta.gateway_origin));
    meta.scope_mask = cred->scope_mask;
    meta.generation = cred->generation;
    meta.expires_at_s = cred->expires_at_s;
    meta.secret_len = (uint32_t)cred->secret_len;
    ESP_RETURN_ON_ERROR(nvs_set_blob(h, meta_key, &meta, sizeof(meta)), TAG, "meta write failed");
    ESP_RETURN_ON_ERROR(nvs_set_blob(h, sec_key, cred->secret, cred->secret_len), TAG, "secret write failed");
    return ESP_OK;
}

static esp_err_t read_cred(nvs_handle_t h, const char *meta_key, const char *sec_key,
                           zv_device_credential_t *cred, bool *present)
{
    *present = false;
    cred_meta_t meta = {0};
    size_t meta_len = sizeof(meta);
    esp_err_t err = nvs_get_blob(h, meta_key, &meta, &meta_len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "meta read failed");
    ESP_RETURN_ON_FALSE(meta_len == sizeof(meta), ESP_ERR_INVALID_SIZE, TAG, "meta size mismatch");
    ESP_RETURN_ON_FALSE(meta.secret_len >= ZV_CRED_SECRET_MIN && meta.secret_len <= ZV_CRED_SECRET_MAX,
                        ESP_ERR_INVALID_SIZE, TAG, "stored secret length invalid");

    size_t sec_len = meta.secret_len;
    ESP_RETURN_ON_ERROR(nvs_get_blob(h, sec_key, cred->secret, &sec_len), TAG, "secret read failed");
    ESP_RETURN_ON_FALSE(sec_len == meta.secret_len, ESP_ERR_INVALID_SIZE, TAG, "secret length mismatch");
    cred->secret[sec_len] = '\0';

    memcpy(cred->device_id, meta.device_id, sizeof(cred->device_id));
    memcpy(cred->gateway_origin, meta.gateway_origin, sizeof(cred->gateway_origin));
    cred->scope_mask = meta.scope_mask;
    cred->generation = meta.generation;
    cred->expires_at_s = meta.expires_at_s;
    cred->secret_len = sec_len;
    *present = true;
    return ESP_OK;
}

esp_err_t zv_storage_load(zv_cred_store_t *store)
{
    ESP_RETURN_ON_FALSE(store != NULL, ESP_ERR_INVALID_ARG, TAG, "null store");
    ESP_RETURN_ON_ERROR(zv_storage_init(), TAG, "storage init failed");
    zv_cred_store_reset(store);

    nvs_handle_t h = 0;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NS, NVS_READONLY, &h), TAG, "NVS open failed");

    bool has = false;
    esp_err_t err = read_cred(h, KEY_ACTIVE_META, KEY_ACTIVE_SECRET, &store->active, &has);
    store->has_active = has;
    if (err == ESP_OK) {
        err = read_cred(h, KEY_STAGED_META, KEY_STAGED_SECRET, &store->staged, &has);
        store->has_staged = has;
    }
    nvs_close(h);
    ESP_RETURN_ON_ERROR(err, TAG, "credential load failed");

    if (store->has_active) {
        char pub[160];
        if (zv_cred_format_public(&store->active, pub, sizeof(pub)) == ZV_OK) {
            ESP_LOGI(TAG, "loaded active credential: %s", pub);
        }
    }
    return ESP_OK;
}

esp_err_t zv_storage_save(const zv_cred_store_t *store)
{
    ESP_RETURN_ON_FALSE(store != NULL, ESP_ERR_INVALID_ARG, TAG, "null store");
    ESP_RETURN_ON_ERROR(zv_storage_init(), TAG, "storage init failed");

    nvs_handle_t h = 0;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NS, NVS_READWRITE, &h), TAG, "NVS open failed");
    esp_err_t err = write_cred(h, KEY_ACTIVE_META, KEY_ACTIVE_SECRET, &store->active, store->has_active);
    if (err == ESP_OK) {
        err = write_cred(h, KEY_STAGED_META, KEY_STAGED_SECRET, &store->staged, store->has_staged);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    ESP_RETURN_ON_ERROR(err, TAG, "credential save failed");
    return ESP_OK;
}

esp_err_t zv_storage_erase(void)
{
    ESP_RETURN_ON_ERROR(zv_storage_init(), TAG, "storage init failed");
    nvs_handle_t h = 0;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NS, NVS_READWRITE, &h), TAG, "NVS open failed");
    (void)nvs_erase_all(h);
    esp_err_t err = nvs_commit(h);
    nvs_close(h);
    ESP_RETURN_ON_ERROR(err, TAG, "credential erase failed");
    ESP_LOGW(TAG, "credential namespace erased");
    return ESP_OK;
}
