/* SPDX-License-Identifier: Apache-2.0 */
#include "zv_platform.h"

#include <stdio.h>
#include <string.h>

#include "esp_app_format.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "sdkconfig.h"
#include "zv_board.h"
#include "zv_identity.h"
#include "zv_ota_policy.h"
/* zv_ui.h declares both the acceptance demo and the interactive shell, so the
   guard has to cover both modes. Scoping it to the acceptance mode alone left
   zv_ui_shell() implicitly declared in a shell build. */
#if CONFIG_ZV_UI_ACCEPTANCE_MODE || CONFIG_ZV_UI_SHELL_MODE
#include "zv_ui.h"
#endif
#if CONFIG_ZV_M1_ACCEPTANCE_MODE
#include "zv_audio.h"
#endif
#if CONFIG_ZV_M2_PROVISION_MODE
#include "zv_provision.h"
#endif
#if CONFIG_ZV_M2_WSS_MODE
#include "zv_net.h"
#endif
#if CONFIG_ZV_M3_APPROVAL_MODE
#include "zv_hermes.h"
#endif
#if CONFIG_ZV_M4_OTA_MODE
#include "zv_ota.h"
#endif

static zv_status_t hardware_gate(void) { return ZV_ERR_HARDWARE_GATE; }

static const char *TAG = "zv_platform";

zv_status_t zv_ui_start(void)
{
#if CONFIG_ZV_UI_SHELL_MODE
    /* The shell selects M0, so the positional display challenge has already run
     * by the time an operator can reach a menu. */
    if (zv_board_m0_acceptance_demo() != ESP_OK) {
        return ZV_ERR_HARDWARE_GATE;
    }
    return zv_ui_shell() == ESP_OK ? ZV_OK : ZV_ERR_HARDWARE_GATE;
#elif CONFIG_ZV_UI_ACCEPTANCE_MODE
    return zv_ui_acceptance_demo() == ESP_OK ? ZV_OK : ZV_ERR_HARDWARE_GATE;
#elif CONFIG_ZV_M0_ACCEPTANCE_MODE
    return zv_board_m0_acceptance_demo() == ESP_OK ? ZV_OK : ZV_ERR_HARDWARE_GATE;
#else
    return hardware_gate();
#endif
}

zv_status_t zv_audio_start(void)
{
#if CONFIG_ZV_M1_ACCEPTANCE_MODE
    /*
     * Bounded M1 audio acceptance only. This never opens a voice path: it
     * measures microphone activity without retaining audio and plays one
     * finite low-volume diagnostic tone, then leaves the amplifier off.
     */
    return zv_audio_m1_acceptance(NULL) == ESP_OK ? ZV_OK : ZV_ERR_HARDWARE_GATE;
#else
    return hardware_gate();
#endif
}

#if CONFIG_ZV_M1_SOAK_MODE
static zv_status_t zv_soak_start(void)
{
    return zv_audio_m1_soak(NULL) == ESP_OK ? ZV_OK : ZV_ERR_HARDWARE_GATE;
}
#endif

zv_status_t zv_avatar_start(void) { return hardware_gate(); }
zv_status_t zv_network_start(void) { return hardware_gate(); }
zv_status_t zv_gateway_start(void)
{
#if CONFIG_ZV_M2_WSS_MODE
    return zv_wss_acceptance_demo() == ESP_OK ? ZV_OK : ZV_ERR_HARDWARE_GATE;
#else
    return hardware_gate();
#endif
}

zv_status_t zv_provisioning_start(void)
{
#if CONFIG_ZV_M2_PROVISION_MODE
    return zv_provision_acceptance_demo() == ESP_OK ? ZV_OK : ZV_ERR_HARDWARE_GATE;
#else
    return hardware_gate();
#endif
}

zv_status_t zv_ota_start(void)
{
#if CONFIG_ZV_M4_OTA_MODE
    /*
     * One bounded A/B update. The pre-download policy, the download
     * accounting, the read-back digest, and the boot-partition switch all run
     * in order; esp_ota_set_boot_partition is reached only when the SHA-256 of
     * what landed in flash matched the manifest. Firmware does not reboot: the
     * operator decides whether to boot a staged image.
     */
    zv_ota_manifest_t manifest;
    memset(&manifest, 0, sizeof(manifest));
    snprintf(manifest.version, sizeof(manifest.version), "%s", CONFIG_ZV_M4_OTA_VERSION);
    snprintf(manifest.board_id, sizeof(manifest.board_id), "%s", ZV_BOARD_ID);
    snprintf(manifest.channel, sizeof(manifest.channel), "%s", CONFIG_ZV_M4_OTA_CHANNEL);
    snprintf(manifest.sha256, sizeof(manifest.sha256), "%s", CONFIG_ZV_M4_OTA_SHA256);
    snprintf(manifest.signature_scheme, sizeof(manifest.signature_scheme), "%s",
             ZV_OTA_SIG_SCHEME_SECURE_BOOT_V2);
    manifest.secure_version = CONFIG_ZV_M4_OTA_SECURE_VERSION;
    manifest.image_size = CONFIG_ZV_M4_OTA_IMAGE_SIZE;

    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    const esp_app_desc_t *app_desc = esp_app_get_description();
    if (running == NULL || target == NULL || app_desc == NULL) {
        return ZV_ERR_HARDWARE_GATE;
    }

    zv_ota_device_t device = {
        /* The running version comes from the app descriptor, so the "not newer"
         * rejection reflects the real installed image rather than a constant. */
        .running_version = app_desc->version,
        .board_id = ZV_BOARD_ID,
        .channel = CONFIG_ZV_M4_OTA_CHANNEL,
        .efuse_secure_version = 0u,
        .partition_size = target->size,
    };

    zv_ota_result_t result;
    esp_err_t err = zv_ota_apply(&manifest, &device, CONFIG_ZV_M4_OTA_URL, &result);
    ESP_LOGI(TAG, "M4 OTA report: running=%s candidate=%s announced=%u received=%u digest_ok=%d sig_ok=%d staged=%d "
                  "slot=%s reason=%s",
             device.running_version, manifest.version, (unsigned)result.image_size, (unsigned)result.downloaded,
             (int)result.digest_match, (int)result.signature_verified, (int)result.boot_partition_set,
             zv_ota_slot_state_name(result.slot_state), zv_ota_reason_name(result.reason));
    return (err == ESP_OK) ? ZV_OK : ZV_ERR_HARDWARE_GATE;
#else
    return hardware_gate();
#endif
}

zv_status_t zv_diagnostics_start(void) { return hardware_gate(); }

zv_status_t zv_hermes_approval_start(void)
{
#if CONFIG_ZV_M3_APPROVAL_MODE
    /*
     * Known-answer self-test only. It proves the mbedtls ES256 verifier and the
     * portable zv_core approval gate agree, including the tamper, replay,
     * unknown-kid, expiry, and retired-key negatives. No Hermes action is
     * executed and no gateway audit is correlated here.
     */
    return zv_hermes_selftest() == ESP_OK ? ZV_OK : ZV_ERR_HARDWARE_GATE;
#else
    return hardware_gate();
#endif
}

/*
 * M0 can only claim the display renders when the operator answered the
 * positional challenge; otherwise the gate says so plainly.
 */
#if CONFIG_ZV_UI_ACCEPTANCE_MODE
static const char *ui_pass_detail(void)
{
    if (zv_board_m0_display_confirmed()) {
        return "M0 display challenge and all eight avatar UI states confirmed by an operator; platform remains "
               "gated until network, gateway, Hermes, credentials, OTA/eFuse, erase, and physical acceptance pass";
    }
    return "eight avatar UI states were painted; display rendering is NOT independently verified; platform remains "
           "gated until network, gateway, Hermes, credentials, OTA/eFuse, erase, and physical acceptance pass";
}
#elif CONFIG_ZV_M1_ACCEPTANCE_MODE
static const char *m1_pass_detail(void)
{
#if CONFIG_ZV_M1_SOAK_MODE
    return "M0, M1, and the bounded M1 soak gates passed; platform remains gated until network, gateway, "
           "Hermes, credentials, OTA/eFuse, erase, and physical acceptance pass";
#else
    if (zv_board_m0_display_confirmed()) {
        return "M0 touch and display confirmation and M1 ES7210/ES8311/NS4150B gates passed; platform remains "
               "gated until network, gateway, Hermes, credentials, OTA/eFuse, erase, and physical acceptance pass";
    }
    return "M0 touch and M1 ES7210/ES8311/NS4150B gates passed; display rendering is NOT verified "
           "by these gates and needs visual operator confirmation; platform remains gated until "
           "network, gateway, Hermes, credentials, OTA/eFuse, erase, and physical acceptance pass";
#endif
}
#elif CONFIG_ZV_M0_ACCEPTANCE_MODE && !CONFIG_ZV_M2_WSS_MODE && !CONFIG_ZV_M2_PROVISION_MODE
static const char *m0_pass_detail(void)
{
    if (zv_board_m0_display_confirmed()) {
        return "M0 touch and display confirmation gates passed; platform remains gated until audio, network, "
               "gateway, Hermes, credentials, OTA/eFuse, erase, and physical acceptance pass";
    }
    return "M0 touch gate passed; display rendering is NOT verified by this gate and needs visual "
           "operator confirmation; platform remains gated until audio, network, gateway, Hermes, "
           "credentials, OTA/eFuse, erase, and physical acceptance pass";
}
#endif

zv_platform_report_t zv_platform_start(void)
{
#if CONFIG_ZV_M3A_VOICE_MODE
    zv_status_t voice_status = zv_gateway_start();
    return (zv_platform_report_t){
        .ready = false,
        .status = voice_status == ZV_OK ? ZV_ERR_HARDWARE_GATE : voice_status,
        .detail = voice_status == ZV_OK ? "M3a voice acceptance loop ended; platform remains gated"
                                         : "M3a voice path failed; platform remains gated",
    };
#elif CONFIG_ZV_M1_ACCEPTANCE_MODE
    /* M1 is built on M0: the display/touch gate runs first, then audio. */
    zv_status_t ui_status = zv_ui_start();
    if (ui_status != ZV_OK) {
        return (zv_platform_report_t){
            .ready = false,
            .status = ui_status,
            .detail = "M0 touch gate failed under M1; audio was not started and all hardware gates remain "
                      "closed",
        };
    }

    zv_status_t audio_status = zv_audio_start();
#if CONFIG_ZV_M1_SOAK_MODE
    if (audio_status != ZV_OK) {
        return (zv_platform_report_t){
            .ready = false,
            .status = audio_status,
            .detail = "M1 audio acceptance failed; soak was not started and all hardware gates remain closed",
        };
    }
    zv_status_t soak_status = zv_soak_start();
    return (zv_platform_report_t){
        .ready = false,
        .status = soak_status == ZV_OK ? ZV_ERR_HARDWARE_GATE : soak_status,
        .detail = soak_status == ZV_OK ? m1_pass_detail()
                                       : "M1 soak failed; all hardware gates remain closed",
    };
#else
    return (zv_platform_report_t){
        .ready = false,
        .status = audio_status == ZV_OK ? ZV_ERR_HARDWARE_GATE : audio_status,
        .detail = audio_status == ZV_OK ? m1_pass_detail()
                                        : "M1 audio acceptance mode failed; all hardware gates remain closed",
    };
#endif
#elif CONFIG_ZV_M4_OTA_MODE
    /* M4 depends on WSS, so dispatch it before the standalone WSS test.
     * The physical M0 challenge must pass before any OTA action. */
    zv_status_t ui_status = zv_ui_start();
    if (ui_status != ZV_OK) {
        return (zv_platform_report_t){
            .ready = false,
            .status = ui_status,
            .detail = "M0 touch gate failed under M4; OTA was not started and all hardware gates remain closed",
        };
    }
    zv_status_t ota_status = zv_ota_start();
    return (zv_platform_report_t){
        .ready = false,
        .status = ota_status == ZV_OK ? ZV_ERR_HARDWARE_GATE : ota_status,
        .detail = ota_status == ZV_OK
                      ? "M0 challenge and one signed A/B OTA stage passed; no image was booted, rollback and "
                        "restore drills are not yet proven, and the platform remains gated"
                      : "M4 OTA acceptance failed; the running image was left as the boot target and all "
                        "hardware gates remain closed",
    };
#elif CONFIG_ZV_M2_WSS_MODE
    zv_status_t wss_status = zv_gateway_start();
    return (zv_platform_report_t){
        .ready = false,
        .status = wss_status == ZV_OK ? ZV_ERR_HARDWARE_GATE : wss_status,
        .detail = wss_status == ZV_OK
                      ? "M0 challenge and M2 WSS hello/reply acceptance passed; platform remains gated until "
                        "session policy, Hermes, OTA, and eFuse acceptance pass"
                      : "M2 WSS acceptance failed; all hardware gates remain closed",
    };
#elif CONFIG_ZV_M2_PROVISION_MODE
    /* M2 provision demo runs the M0 challenge itself, then the store gates. */
    zv_status_t prov_status = zv_provisioning_start();
    return (zv_platform_report_t){
        .ready = false,
        .status = prov_status == ZV_OK ? ZV_ERR_HARDWARE_GATE : prov_status,
        .detail = prov_status == ZV_OK
                      ? "M0 challenge and M2 encrypted credential store acceptance passed; platform remains gated "
                        "until network, gateway, Hermes, OTA, and eFuse acceptance pass"
                      : "M2 provision acceptance failed; all hardware gates remain closed",
    };
#elif CONFIG_ZV_M3_APPROVAL_MODE
    /* M3 selects M0: the challenge runs first so an operator is present. */
    zv_status_t ui_status = zv_ui_start();
    if (ui_status != ZV_OK) {
        return (zv_platform_report_t){
            .ready = false,
            .status = ui_status,
            .detail = "M0 touch gate failed under M3; the approval self-test was not started and all "
                      "hardware gates remain closed",
        };
    }
    zv_status_t approval_status = zv_hermes_approval_start();
    return (zv_platform_report_t){
        .ready = false,
        .status = approval_status == ZV_OK ? ZV_ERR_HARDWARE_GATE : approval_status,
        .detail = approval_status == ZV_OK
                      ? "M0 challenge and the M3 ES256 known-answer self-test passed; this does NOT prove a "
                        "live gateway approval, Hermes execution, or audit correlation, and the platform "
                        "remains gated"
                      : "M3 approval self-test failed; all hardware gates remain closed",
    };
#elif CONFIG_ZV_UI_SHELL_MODE
    /* The shell selects M0; the challenge runs before zv_ui_start(). */
    zv_status_t ui_status = zv_ui_start();
    return (zv_platform_report_t){
        .ready = false,
        .status = ui_status == ZV_OK ? ZV_ERR_HARDWARE_GATE : ui_status,
        .detail = ui_status == ZV_OK
                      ? "the M0 challenge passed and the operator drove the interactive shell (menu, volume, "
                        "mute, confirmed restart); the ES8311 is NOT yet driven from the shell volume state, and "
                        "network, gateway, Hermes, credentials, OTA/eFuse and erase remain gated"
                      : "UI shell mode failed; all hardware gates remain closed",
    };
#elif CONFIG_ZV_UI_ACCEPTANCE_MODE
    /* UI mode selects M0; the challenge runs inside the UI demo. */
    zv_status_t ui_status = zv_ui_start();
    return (zv_platform_report_t){
        .ready = false,
        .status = ui_status == ZV_OK ? ZV_ERR_HARDWARE_GATE : ui_status,
        .detail = ui_status == ZV_OK ? ui_pass_detail()
                                     : "UI acceptance mode failed; all hardware gates remain closed",
    };
#elif CONFIG_ZV_M0_ACCEPTANCE_MODE
    zv_status_t ui_status = zv_ui_start();
    return (zv_platform_report_t){
        .ready = false,
        .status = ui_status == ZV_OK ? ZV_ERR_HARDWARE_GATE : ui_status,
        .detail = ui_status == ZV_OK ? m0_pass_detail()
                                     : "M0 acceptance mode failed; all hardware gates remain closed",
    };
#else
    return (zv_platform_report_t){
        .ready = false,
        .status = ZV_ERR_HARDWARE_GATE,
        .detail = "Rev2 hardware drivers are not accepted; run M0/M1/UI gates",
    };
#endif
}
