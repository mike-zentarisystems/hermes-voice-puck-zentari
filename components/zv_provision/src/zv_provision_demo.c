/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 */
#include "zv_provision.h"

#if CONFIG_ZV_M2_PROVISION_MODE

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "zv_board.h"
#include "zv_cred_rotation.h"
#include "zv_cred_store.h"
#include "zv_storage.h"

static const char *TAG = "zv_provision";

#define PRESENCE_POLL_MS 50u
#define PRESENCE_HOLD_MS 1500u

static zv_device_credential_t make_fixture(uint32_t generation)
{
    zv_device_credential_t c;
    memset(&c, 0, sizeof(c));
    /* Inert lab fixture only — never a real secret. */
    memcpy(c.device_id, "puck-lab-0001", sizeof("puck-lab-0001"));
    memcpy(c.gateway_origin, "wss://gateway.invalid/ws", sizeof("wss://gateway.invalid/ws"));
    memcpy(c.secret, "fixture-nonsecret-00000000000000000000000000000000", 50);
    c.secret_len = 50;
    c.scope_mask = (1u << ZV_SCOPE_SESSION_CONNECT) | (1u << ZV_SCOPE_AUDIO_STREAM) | (1u << ZV_SCOPE_EVENTS_RECEIVE);
    c.generation = generation;
    c.expires_at_s = 4102444800;
    return c;
}

static esp_err_t require_physical_presence(void)
{
    ESP_RETURN_ON_ERROR(zv_board_touch_wait_release(5000), TAG, "finger still down; release to arm");
    ESP_LOGW(TAG, "physical presence required: touch and HOLD the panel for %d ms", PRESENCE_HOLD_MS);

    uint32_t held = 0;
    for (;;) {
        bool touched = false;
        ESP_RETURN_ON_ERROR(zv_board_touch_poll(&touched, NULL, NULL), TAG, "touch poll failed");
        if (!touched) {
            held = 0;
        } else {
            held += PRESENCE_POLL_MS;
            if (held >= PRESENCE_HOLD_MS) {
                ESP_LOGI(TAG, "physical presence confirmed (%u ms hold)", (unsigned)held);
                return ESP_OK;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(PRESENCE_POLL_MS));
    }
}

esp_err_t zv_provision_acceptance_demo(void)
{
    /*
     * zv_cred_store_t is ~1.5 KB (two full credentials with 512-byte secrets).
     * Several stack locals overflowed the main task and tripped the interrupt
     * WDT mid-log. Keep every large object static.
     */
    static zv_cred_store_t store;
    static zv_device_credential_t gen1;
    static zv_device_credential_t replay;
    static zv_device_credential_t gen2;
    static zv_rotation_t rot;
    char pub[160];

    ESP_LOGW(TAG, "M2 provision acceptance enabled: encrypted NVS credential + rotation policy only; "
                  "network, gateway, Hermes, OTA, eFuse remain gated");

    ESP_RETURN_ON_ERROR(zv_board_m0_acceptance_demo(), TAG, "M0 challenge failed under M2 provision mode");
    ESP_RETURN_ON_ERROR(zv_board_touch_init(), TAG, "touch re-init failed");
    ESP_RETURN_ON_ERROR(require_physical_presence(), TAG, "physical presence gate failed");

    ESP_RETURN_ON_ERROR(zv_storage_init(), TAG, "encrypted NVS init failed");
    ESP_RETURN_ON_ERROR(zv_storage_erase(), TAG, "clear previous credentials failed");

    zv_cred_store_reset(&store);
    gen1 = make_fixture(1);
    ESP_RETURN_ON_ERROR(zv_cred_store_install_active(&store, &gen1) == ZV_OK ? ESP_OK : ESP_ERR_INVALID_STATE, TAG,
                        "install gen1 failed");
    ESP_RETURN_ON_ERROR(zv_storage_save(&store), TAG, "save gen1 failed");

    zv_cred_store_reset(&store);
    ESP_RETURN_ON_ERROR(zv_storage_load(&store), TAG, "reload failed");
    ESP_RETURN_ON_FALSE(store.has_active && store.active.generation == 1u, ESP_ERR_INVALID_RESPONSE, TAG,
                        "reload did not produce active generation 1");
    ESP_RETURN_ON_FALSE(store.active.secret_len == gen1.secret_len &&
                            memcmp(store.active.secret, gen1.secret, gen1.secret_len) == 0,
                        ESP_ERR_INVALID_RESPONSE, TAG, "reloaded secret does not match (encrypted store broken)");
    ESP_RETURN_ON_ERROR(zv_cred_format_public(&store.active, pub, sizeof(pub)) == ZV_OK ? ESP_OK : ESP_FAIL, TAG,
                        "public format failed");
    ESP_LOGI(TAG, "reloaded: %s", pub);

    /* Rollback: generation equal to active must be refused. */
    replay = make_fixture(1);
    ESP_RETURN_ON_FALSE(zv_cred_store_stage(&store, &replay) == ZV_ERR_POLICY, ESP_ERR_INVALID_STATE, TAG,
                        "replayed generation was not refused");

    gen2 = make_fixture(2);
    ESP_RETURN_ON_ERROR(zv_cred_store_stage(&store, &gen2) == ZV_OK ? ESP_OK : ESP_ERR_INVALID_STATE, TAG,
                        "stage gen2 failed");
    ESP_RETURN_ON_ERROR(zv_storage_save(&store), TAG, "save staged failed");

    ESP_RETURN_ON_ERROR(zv_rotation_init(&rot, 1u) == ZV_OK ? ESP_OK : ESP_FAIL, TAG, "rotation init failed");
    ESP_RETURN_ON_ERROR(zv_rotation_stage(&rot, 2u, 15000u) == ZV_OK ? ESP_OK : ESP_FAIL, TAG, "rotation stage failed");
    ESP_RETURN_ON_ERROR(zv_rotation_begin_verify(&rot, 0u) == ZV_OK ? ESP_OK : ESP_FAIL, TAG, "rotation verify failed");
    ESP_RETURN_ON_ERROR(zv_rotation_on_verified(&rot) == ZV_OK ? ESP_OK : ESP_FAIL, TAG, "rotation commit failed");
    ESP_RETURN_ON_ERROR(zv_cred_store_commit_staged(&store) == ZV_OK ? ESP_OK : ESP_FAIL, TAG, "store commit failed");
    ESP_RETURN_ON_ERROR(zv_storage_save(&store), TAG, "save committed failed");

    zv_cred_store_reset(&store);
    ESP_RETURN_ON_ERROR(zv_storage_load(&store), TAG, "reload after commit failed");
    ESP_RETURN_ON_FALSE(store.has_active && store.active.generation == 2u && !store.has_staged,
                        ESP_ERR_INVALID_RESPONSE, TAG, "committed rotation not persisted");

    ESP_RETURN_ON_ERROR(zv_storage_erase(), TAG, "final erase failed");
    zv_cred_store_reset(&store);
    ESP_RETURN_ON_ERROR(zv_storage_load(&store), TAG, "reload after erase failed");
    ESP_RETURN_ON_FALSE(!store.has_active && !store.has_staged, ESP_ERR_INVALID_RESPONSE, TAG,
                        "credentials remain after erase");

    ESP_LOGI(TAG, "M2 provision acceptance passed; platform remains gated");
    return ESP_OK;
}

#else /* !CONFIG_ZV_M2_PROVISION_MODE */

esp_err_t zv_provision_acceptance_demo(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

#endif /* CONFIG_ZV_M2_PROVISION_MODE */
