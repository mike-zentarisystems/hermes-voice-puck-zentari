/* SPDX-License-Identifier: Apache-2.0 */
#include "zv_net.h"

#if CONFIG_ZV_M2_WSS_MODE
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "mbedtls/md.h"
#include "sdkconfig.h"
#include "zv_board.h"
#if CONFIG_ZV_M3A_VOICE_MODE
#include "cJSON.h"
#include "zv_audio.h"
#include "zv_app_state.h"
#include "zv_ui_draw.h"
#endif
#include "zv_cred_store.h"
#include "zv_gateway.h"
#include "zv_identity.h"
#include "zv_link_acceptance.h"
#include "zv_link_supervisor.h"
#include "zv_storage.h"
#include "zv_ui.h"
#include "zv_power.h"

static const char *TAG = "zv_net";
#if CONFIG_ZV_M3A_VOICE_MODE
#define ZV_VOICE_ENABLED 1
#else
#define ZV_VOICE_ENABLED 0
#endif
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_DISCONNECTED_BIT BIT1
#define WS_CONNECTED_BIT BIT2
#define WS_TEXT_BIT BIT3
#define WS_FAIL_BIT BIT4
#define WIFI_ASSOCIATED_BIT BIT5
#define CLOCK_RESTART_BIT BIT6
#define WIFI_TIMEOUT_MS 20000u
#define WS_TIMEOUT_MS 15000u
#define LINK_BUDGET_MS 90000u
#define POLL_MS 100u
#define MEMORY_SAMPLE_MS 30000u
#define BOOT_SCREEN_HOLD_MS 2000u

static EventGroupHandle_t s_events;
static char s_rx[512];
static char s_tx[512];
#if CONFIG_ZV_M3A_VOICE_MODE
#define VOICE_QUEUE_DEPTH 8u
#define VOICE_FRAME_MAX 4096u
typedef struct { char json[VOICE_FRAME_MAX + 1u]; } voice_frame_t;
static QueueHandle_t s_voice_queue;
static StaticQueue_t s_voice_queue_control;
static uint8_t *s_voice_queue_storage;
static volatile bool s_voice_active;
static bool s_voice_discarding;
static portMUX_TYPE s_voice_queue_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_voice_queue_callbacks;
static voice_frame_t s_voice_incoming;
/* Voice loop is single-owner; bounded buffers stay off the main-task stack. */
static unsigned char s_voice_chunk_encoded[700];
static char s_voice_chunk_json[1024];
static int16_t s_voice_output_pcm[512];
static int16_t s_voice_input_pcm[256];
#endif
typedef struct {
    zv_link_memory_snapshot_t baseline;
    zv_link_memory_snapshot_t last;
    zv_link_ui_evidence_t ui;
    uint32_t last_memory_ms;
    uint32_t memory_samples;
    bool memory_ok;
} link_acceptance_evidence_t;
static link_acceptance_evidence_t s_acceptance;
static const zv_link_memory_limits_t s_memory_limits = {
    .internal_floor = 64u * 1024u, .internal_largest_floor = 32u * 1024u,
    .dma_floor = 64u * 1024u, .dma_largest_floor = 16u * 1024u,
    .max_internal_drop = 32u * 1024u, .max_dma_drop = 32u * 1024u,
};

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static uint32_t link_random(void *ctx) { (void)ctx; return esp_random(); }

static uint8_t wifi_signal_bars(void)
{
    wifi_ap_record_t ap = {0};
    if (s_events == NULL || (xEventGroupGetBits(s_events) & WIFI_ASSOCIATED_BIT) == 0u ||
        esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return 0u;
    }
    if (ap.rssi >= -55) {
        return 3u;
    }
    if (ap.rssi >= -70) {
        return 2u;
    }
    return 1u;
}

static esp_err_t show_link_state(zv_link_state_t state, uint32_t retry_ms)
{
    esp_err_t err = zv_ui_show_link_state(state, retry_ms, wifi_signal_bars());
    zv_link_ui_record(&s_acceptance.ui, state, err == ESP_OK);
    return err;
}

static void sample_memory(bool force)
{
    const uint32_t now = now_ms();
    if (!force && s_acceptance.memory_samples != 0u &&
        (uint32_t)(now - s_acceptance.last_memory_ms) < MEMORY_SAMPLE_MS) {
        return;
    }
    zv_link_memory_snapshot_t sample = {
        .internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        .internal_min_free = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
        .internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
        .dma_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
        .dma_min_free = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
        .dma_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
    };
    if (s_acceptance.memory_samples == 0u) {
        s_acceptance.baseline = sample;
        s_acceptance.memory_ok = true;
    } else if (!zv_link_memory_stable(&s_acceptance.baseline, &sample, &s_memory_limits)) {
        s_acceptance.memory_ok = false;
    }
    s_acceptance.last = sample;
    s_acceptance.last_memory_ms = now;
    s_acceptance.memory_samples++;
    ESP_LOGI(TAG, "heap sample=%lu internal_free=%u internal_min=%u internal_largest=%u dma_free=%u dma_min=%u dma_largest=%u stable=%s",
             (unsigned long)s_acceptance.memory_samples, (unsigned)sample.internal_free,
             (unsigned)sample.internal_min_free, (unsigned)sample.internal_largest,
             (unsigned)sample.dma_free, (unsigned)sample.dma_min_free,
             (unsigned)sample.dma_largest, s_acceptance.memory_ok ? "yes" : "no");
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "link_diag event=wifi_got_ip uptime_ms=%llu",
                 (unsigned long long)(esp_timer_get_time() / 1000));
        xEventGroupClearBits(s_events, WIFI_DISCONNECTED_BIT);
        xEventGroupSetBits(s_events, WIFI_CONNECTED_BIT | CLOCK_RESTART_BIT);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) {
        ESP_LOGW(TAG, "link_diag event=wifi_lost_ip uptime_ms=%llu",
                 (unsigned long long)(esp_timer_get_time() / 1000));
        xEventGroupClearBits(s_events, WIFI_CONNECTED_BIT);
        xEventGroupSetBits(s_events, WIFI_DISCONNECTED_BIT);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        ESP_LOGI(TAG, "link_diag event=wifi_connected uptime_ms=%llu",
                 (unsigned long long)(esp_timer_get_time() / 1000));
        xEventGroupClearBits(s_events, WIFI_DISCONNECTED_BIT);
        xEventGroupSetBits(s_events, WIFI_ASSOCIATED_BIT);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        /* Never log this struct's SSID/BSSID; only the numeric reason. */
        const wifi_event_sta_disconnected_t *event = data;
        ESP_LOGW(TAG, "link_diag event=wifi_disconnected reason=%d uptime_ms=%llu",
                 event != NULL ? (int)event->reason : -1,
                 (unsigned long long)(esp_timer_get_time() / 1000));
        /* The supervisor, not the Wi-Fi driver, owns reconnect timing. */
        xEventGroupClearBits(s_events, WIFI_CONNECTED_BIT | WIFI_ASSOCIATED_BIT);
        xEventGroupSetBits(s_events, WIFI_DISCONNECTED_BIT);
    }
}

static void ws_event_handler(void *arg, esp_event_base_t base, int32_t id, void *event_data)
{
    (void)arg; (void)base;
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    if (id == WEBSOCKET_EVENT_CONNECTED) {
        ESP_LOGI(TAG, "link_diag event=ws_connected uptime_ms=%llu",
                 (unsigned long long)(esp_timer_get_time() / 1000));
        xEventGroupSetBits(s_events, WS_CONNECTED_BIT);
    } else if (id == WEBSOCKET_EVENT_DATA && data != NULL) {
        if (data->op_code == 0x8) {
            /* Only the two-byte status code; the following reason is untrusted. */
            int code = 0;
            if (data->data_ptr != NULL && data->payload_offset == 0 && data->data_len >= 2) {
                code = ((int)(uint8_t)data->data_ptr[0] << 8) | (int)(uint8_t)data->data_ptr[1];
            }
            ESP_LOGW(TAG, "link_diag event=ws_close_frame close_code=%d uptime_ms=%llu",
                     code, (unsigned long long)(esp_timer_get_time() / 1000));
        }
        /* PING/PONG are transport liveness frames, not gateway replies. */
        if (data->op_code == 0x9 || data->op_code == 0xA) {
            return;
        }
#if CONFIG_ZV_M3A_VOICE_MODE
        QueueHandle_t voice_queue = NULL;
        portENTER_CRITICAL(&s_voice_queue_lock);
        bool voice_active = s_voice_active;
        bool voice_discarding = s_voice_discarding;
        if (voice_active && s_voice_queue != NULL) {
            voice_queue = s_voice_queue;
            s_voice_queue_callbacks++;
        }
        portEXIT_CRITICAL(&s_voice_queue_lock);
        if (voice_discarding) return;
        if (voice_active) {
            if (voice_queue != NULL && data->data_ptr != NULL &&
                zv_link_reply_frame_valid((uint8_t)data->op_code, data->fin, data->payload_len,
                                          data->payload_offset, data->data_len, VOICE_FRAME_MAX + 1u)) {
                memcpy(s_voice_incoming.json, data->data_ptr, (size_t)data->data_len);
                s_voice_incoming.json[data->data_len] = '\0';
                /* Bounded TCP backpressure during display paints/audio writes.
                 * Queue ownership remains pinned until this callback exits. */
                if (xQueueSend(voice_queue, &s_voice_incoming, pdMS_TO_TICKS(1000u)) != pdTRUE) {
                    ESP_LOGW(TAG, "voice receive failed: category=rx_queue_full");
                    xEventGroupSetBits(s_events, WS_FAIL_BIT);
                }
            } else {
                xEventGroupSetBits(s_events, WS_FAIL_BIT);
            }
            if (voice_queue != NULL) {
                portENTER_CRITICAL(&s_voice_queue_lock);
                s_voice_queue_callbacks--;
                portEXIT_CRITICAL(&s_voice_queue_lock);
            }
            return;
        }
#endif
        if (data->data_ptr != NULL &&
            zv_link_reply_frame_valid((uint8_t)data->op_code, data->fin, data->payload_len,
                                      data->payload_offset, data->data_len, sizeof(s_rx))) {
            memcpy(s_rx, data->data_ptr, (size_t)data->data_len);
            s_rx[data->data_len] = '\0';
            xEventGroupSetBits(s_events, WS_TEXT_BIT);
        } else {
            xEventGroupSetBits(s_events, WS_FAIL_BIT);
        }
    } else if (id == WEBSOCKET_EVENT_DISCONNECTED || id == WEBSOCKET_EVENT_ERROR) {
        ESP_LOGW(TAG, "link_diag event=%s close_code=%d error_type=%d tls_error=%d socket_errno=%d uptime_ms=%llu",
                 id == WEBSOCKET_EVENT_ERROR ? "ws_error" : "ws_disconnected",
                 data != NULL ? data->close_status_code : 0,
                 data != NULL ? (int)data->error_handle.error_type : -1,
                 data != NULL ? (int)data->error_handle.esp_tls_last_esp_err : 0,
                 data != NULL ? data->error_handle.esp_transport_sock_errno : 0,
                 (unsigned long long)(esp_timer_get_time() / 1000));
        xEventGroupSetBits(s_events, WS_FAIL_BIT);
    }
}

static esp_err_t wifi_start(void)
{
    const char *ssid = CONFIG_ZV_M2_WIFI_SSID;
    const char *pass = CONFIG_ZV_M2_WIFI_PASSWORD;
    ESP_RETURN_ON_FALSE(ssid != NULL && ssid[0] != '\0', ESP_ERR_INVALID_STATE, TAG,
                        "Wi-Fi configuration missing; refusing network start");
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init failed");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop failed");
    ESP_RETURN_ON_FALSE(esp_netif_create_default_wifi_sta() != NULL, ESP_ERR_NO_MEM, TAG, "STA netif failed");
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), TAG, "Wi-Fi init failed");
    /* IDF wifi INFO prints the SSID ("connected with ..."); keep it out of operator logs. */
    esp_log_level_set("wifi", ESP_LOG_WARN);
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL), TAG,
                        "Wi-Fi handler failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL), TAG,
                        "IP handler failed");
    wifi_config_t cfg = {0};
    (void)strncpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid) - 1u);
    if (pass != NULL) (void)strncpy((char *)cfg.sta.password, pass, sizeof(cfg.sta.password) - 1u);
    cfg.sta.threshold.authmode = pass != NULL && pass[0] != '\0' ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "Wi-Fi mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &cfg), TAG, "Wi-Fi config failed");
    return esp_wifi_start();
}

static bool wifi_has_ip(void)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif == NULL) {
        return false;
    }
    esp_netif_ip_info_t ip = {0};
    return esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0u;
}

static void clock_synced(struct timeval *tv)
{
    (void)tv;
    ESP_LOGI(TAG, "clock synchronized");
}

/* Called only by the link task, after IP is available. IP events request a
 * restart so SNTP does not retain a long retry delay across a Wi-Fi outage. */
static void clock_ensure_started(void)
{
    static bool started;
    const bool restart = (xEventGroupClearBits(s_events, CLOCK_RESTART_BIT) & CLOCK_RESTART_BIT) != 0u;
    esp_err_t err = ESP_OK;
    if (!started) {
        esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        config.sync_cb = clock_synced;
        err = esp_netif_sntp_init(&config);
        started = err == ESP_OK;
    } else if (restart) {
        err = esp_netif_sntp_start();
    }
    if (err != ESP_OK) {
        xEventGroupSetBits(s_events, CLOCK_RESTART_BIT);
        ESP_LOGW(TAG, "clock sync start unavailable; retry on next connection");
    }
}

static esp_err_t wifi_connect_attempt(void)
{
    if ((xEventGroupGetBits(s_events) & WIFI_CONNECTED_BIT) != 0u) {
        clock_ensure_started();
        return ESP_OK;
    }
    /* A cleared event bit must not force a join while the STA is still
       associated: that path disconnects first ("sta is connected, disconnect
       before connecting to new ap") and then burns the full join timeout,
       leaving the supervisor stuck offline/degraded with a live AP. */
    if (wifi_has_ip()) {
        xEventGroupClearBits(s_events, WIFI_DISCONNECTED_BIT);
        xEventGroupSetBits(s_events, WIFI_CONNECTED_BIT);
        clock_ensure_started();
        return ESP_OK;
    }
    EventBits_t current = xEventGroupGetBits(s_events);
    wifi_ap_record_t ap = {0};
    const bool associated = (current & WIFI_ASSOCIATED_BIT) != 0u ||
                            esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
    xEventGroupClearBits(s_events, WIFI_DISCONNECTED_BIT);
    esp_err_t err = associated ? ESP_OK : esp_wifi_connect();
    if (err != ESP_OK) {
        return err;
    }
    EventBits_t bits = xEventGroupWaitBits(s_events, WIFI_CONNECTED_BIT | WIFI_DISCONNECTED_BIT, pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(WIFI_TIMEOUT_MS));
    if ((bits & WIFI_CONNECTED_BIT) == 0u && wifi_has_ip()) {
        xEventGroupClearBits(s_events, WIFI_DISCONNECTED_BIT);
        xEventGroupSetBits(s_events, WIFI_CONNECTED_BIT);
        bits = WIFI_CONNECTED_BIT;
    }
    if ((bits & WIFI_CONNECTED_BIT) == 0u || (bits & WIFI_DISCONNECTED_BIT) != 0u) return ESP_ERR_TIMEOUT;
    ESP_LOGI(TAG, "Wi-Fi joined (network name redacted)");
    clock_ensure_started();
    return ESP_OK;
}

static void rand_uuid(char out[37])
{
    static const char *hex = "0123456789abcdef"; uint8_t raw[16]; int n = 0;
    esp_fill_random(raw, sizeof(raw)); raw[6] = (uint8_t)((raw[6] & 15u) | 0x40u); raw[8] = (uint8_t)((raw[8] & 63u) | 0x80u);
    for (int i = 0; i < 16; i++) { if (i == 4 || i == 6 || i == 8 || i == 10) out[n++] = '-'; out[n++] = hex[raw[i] >> 4]; out[n++] = hex[raw[i] & 15u]; }
    out[n] = '\0';
}

static bool json_string(const char *json, const char *name, char *out, size_t out_size)
{
    char needle[48];
    int n = snprintf(needle, sizeof(needle), "\"%s\":\"", name);
    if (json == NULL || out == NULL || out_size == 0u || n <= 0 || (size_t)n >= sizeof(needle)) return false;
    const char *start = strstr(json, needle);
    if (start == NULL) return false;
    start += (size_t)n;
    const char *end = start;
    while (*end != '\0' && *end != '"') {
        if (*end == '\\' || (unsigned char)*end < 0x20u) return false;
        end++;
    }
    size_t len = (size_t)(end - start);
    if (*end != '"' || len == 0u || len >= out_size) return false;
    memcpy(out, start, len);
    out[len] = '\0';
    return true;
}

static esp_err_t build_auth_proof(const char *challenge_json, const char *hello_id,
                                  const zv_device_credential_t *credential,
                                  char *out, size_t out_size)
{
    ESP_RETURN_ON_FALSE(challenge_json != NULL && hello_id != NULL && credential != NULL &&
                            out != NULL && out_size > 0u,
                        ESP_ERR_INVALID_ARG, TAG, "auth proof arguments invalid");
    char type[24], challenge_id[40], nonce[72], bound_hello_id[40];
    bool valid = strstr(challenge_json, "\"v\":1") != NULL &&
                 strstr(challenge_json, "\"expires_ms\":") != NULL &&
                 json_string(challenge_json, "type", type, sizeof(type)) &&
                 strcmp(type, "auth.challenge") == 0 &&
                 json_string(challenge_json, "challenge_id", challenge_id, sizeof(challenge_id)) &&
                 zv_hello_id_is_uuid(challenge_id) &&
                 json_string(challenge_json, "nonce", nonce, sizeof(nonce)) &&
                 strlen(nonce) >= 43u && strlen(nonce) <= 64u &&
                 json_string(challenge_json, "hello_id", bound_hello_id, sizeof(bound_hello_id)) &&
                 strcmp(bound_hello_id, hello_id) == 0;
    if (!valid) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    char canonical[256];
    int canonical_len = snprintf(canonical, sizeof(canonical), "zv1\n%s\n%s\n%s\n%lu\n%s",
                                 challenge_id, nonce, credential->device_id,
                                 (unsigned long)credential->generation, hello_id);
    if (canonical_len <= 0 || (size_t)canonical_len >= sizeof(canonical)) {
        return ESP_ERR_INVALID_SIZE;
    }
    unsigned char digest[32];
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    int rc = md == NULL ? -1 : mbedtls_md_hmac(md,
                                               (const unsigned char *)credential->secret,
                                               credential->secret_len,
                                               (const unsigned char *)canonical,
                                               (size_t)canonical_len, digest);
    memset(canonical, 0, sizeof(canonical));
    if (rc != 0) {
        return ESP_FAIL;
    }
    unsigned char encoded[48];
    size_t encoded_len = 0u;
    rc = mbedtls_base64_encode(encoded, sizeof(encoded), &encoded_len, digest, sizeof(digest));
    memset(digest, 0, sizeof(digest));
    if (rc != 0 || encoded_len == 0u || encoded_len >= sizeof(encoded)) {
        return ESP_FAIL;
    }
    while (encoded_len > 0u && encoded[encoded_len - 1u] == '=') encoded_len--;
    for (size_t i = 0u; i < encoded_len; i++) {
        if (encoded[i] == '+') encoded[i] = '-';
        else if (encoded[i] == '/') encoded[i] = '_';
    }
    encoded[encoded_len] = '\0';
    int written = snprintf(out, out_size,
                           "{\"v\":1,\"type\":\"auth.prove\",\"challenge_id\":\"%s\","
                           "\"hello_id\":\"%s\",\"device_id\":\"%s\",\"generation\":%lu,"
                           "\"proof\":\"%s\"}",
                           challenge_id, hello_id, credential->device_id,
                           (unsigned long)credential->generation, encoded);
    memset(encoded, 0, sizeof(encoded));
    return written > 0 && (size_t)written < out_size ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static bool session_accepted(const char *json, const char *hello_id,
                             const zv_device_credential_t *credential)
{
    char type[24], device_id[ZV_CRED_DEVICE_ID_MAX + 1u], correlation_id[40], session_id[40];
    return strstr(json, "\"v\":1") != NULL &&
           json_string(json, "type", type, sizeof(type)) && strcmp(type, "session.accepted") == 0 &&
           json_string(json, "device_id", device_id, sizeof(device_id)) && strcmp(device_id, credential->device_id) == 0 &&
           json_string(json, "correlation_id", correlation_id, sizeof(correlation_id)) && strcmp(correlation_id, hello_id) == 0 &&
           json_string(json, "session_id", session_id, sizeof(session_id)) && zv_hello_id_is_uuid(session_id);
}

#if CONFIG_ZV_M3A_VOICE_MODE
typedef enum { VOICE_IDLE, VOICE_LISTENING, VOICE_THINKING, VOICE_SPEAKING } voice_state_t;
typedef struct {
    voice_state_t state;
    char correlation_id[37];
    uint32_t tx_seq;
    uint32_t rx_seq;
    uint32_t started_ms;
    uint32_t stage_started_ms;
    size_t input_bytes;
    size_t output_bytes;
    bool transcript_seen;
    bool thinking_seen;
    bool text_seen;
} voice_turn_t;
static voice_frame_t s_voice_processing;

/* Gateway STT/TTS stages are bounded at 30 s; the authenticated agent adapter
 * supports up to 89 s (90 s including its stage wrapper). Allow delivery grace
 * so the gateway can return its categorized error before we close the link.
 * This does not reset on chunks or change the gateway's 150 s whole-turn cap. */
static bool voice_turn_expired(const voice_turn_t *turn, uint32_t stamp)
{
    uint32_t stage_ms = 35000u;
    if (turn->state == VOICE_IDLE) return false;
    if (turn->state == VOICE_SPEAKING) stage_ms = 65000u;
    else if (turn->state == VOICE_THINKING && turn->thinking_seen && !turn->text_seen)
        stage_ms = 95000u;
    return (uint32_t)(stamp - turn->stage_started_ms) >= stage_ms ||
           (uint32_t)(stamp - turn->started_ms) >= 155000u;
}

static esp_err_t voice_send(esp_websocket_client_handle_t client, const char *json)
{
    size_t length = strlen(json);
    if (length == 0u || length > VOICE_FRAME_MAX) return ESP_ERR_INVALID_SIZE;
    int sent = esp_websocket_client_send_text(client, json, (int)length, pdMS_TO_TICKS(1000));
    return sent == (int)length ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

static esp_err_t voice_send_event(esp_websocket_client_handle_t client,
                                  const zv_device_credential_t *credential,
                                  const char *session_id, voice_turn_t *turn,
                                  const char *type, bool start)
{
    char json[384];
    int n = snprintf(json, sizeof(json),
                     "{\"v\":1,\"type\":\"%s\",\"device_id\":\"%s\",\"session_id\":\"%s\","
                     "\"correlation_id\":\"%s\",\"seq\":%lu%s}",
                     type, credential->device_id, session_id, turn->correlation_id,
                     (unsigned long)++turn->tx_seq,
                     start ? ",\"format\":{\"encoding\":\"pcm_s16le\",\"sample_rate\":16000,\"channels\":1}" : "");
    return n > 0 && (size_t)n < sizeof(json) ? voice_send(client, json) : ESP_ERR_INVALID_SIZE;
}

static esp_err_t voice_send_chunk(esp_websocket_client_handle_t client,
                                  const zv_device_credential_t *credential,
                                  const char *session_id, voice_turn_t *turn,
                                  const int16_t pcm[256])
{
    size_t length = 0;
    int rc = mbedtls_base64_encode(s_voice_chunk_encoded, sizeof(s_voice_chunk_encoded), &length,
                                   (const unsigned char *)pcm, 512u);
    if (rc != 0 || length == 0u || length >= sizeof(s_voice_chunk_encoded)) return ESP_ERR_INVALID_SIZE;
    s_voice_chunk_encoded[length] = '\0';
    int n = snprintf(s_voice_chunk_json, sizeof(s_voice_chunk_json),
                     "{\"v\":1,\"type\":\"audio.input.chunk\",\"device_id\":\"%s\","
                     "\"session_id\":\"%s\",\"correlation_id\":\"%s\",\"seq\":%lu,\"data\":\"%s\"}",
                     credential->device_id, session_id, turn->correlation_id,
                     (unsigned long)++turn->tx_seq, s_voice_chunk_encoded);
    memset(s_voice_chunk_encoded, 0, sizeof(s_voice_chunk_encoded));
    return n > 0 && (size_t)n < sizeof(s_voice_chunk_json) ? voice_send(client, s_voice_chunk_json) : ESP_ERR_INVALID_SIZE;
}

static bool voice_format_valid(const cJSON *format)
{
    const cJSON *encoding = cJSON_GetObjectItemCaseSensitive(format, "encoding");
    const cJSON *rate = cJSON_GetObjectItemCaseSensitive(format, "sample_rate");
    const cJSON *channels = cJSON_GetObjectItemCaseSensitive(format, "channels");
    unsigned count = 0;
    for (const cJSON *item = format == NULL ? NULL : format->child; item != NULL; item = item->next) count++;
    return count == 3u && cJSON_IsString(encoding) && strcmp(encoding->valuestring, "pcm_s16le") == 0 &&
           cJSON_IsNumber(rate) && rate->valuedouble == 16000.0 &&
           cJSON_IsNumber(channels) && channels->valuedouble == 1.0;
}

static bool voice_error_code_valid(const char *code)
{
    if (code == NULL) return false;
    size_t length = strlen(code);
    if (length == 0u || length > 48u) return false;
    for (size_t i = 0; i < length; ++i) {
        char c = code[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    }
    return true;
}

static const char *voice_error_category(const char *code)
{
    static const char *const categories[] = {
        "stt_failed", "stt_timeout", "stt_unavailable",
        "hermes_failed", "hermes_timeout", "hermes_readonly_unavailable",
        "tts_failed", "tts_timeout", "tts_unavailable", "turn_timeout"
    };
    for (size_t i = 0; i < sizeof(categories) / sizeof(categories[0]); ++i) {
        if (strcmp(code, categories[i]) == 0) return categories[i];
    }
    return "gateway_error";
}

static esp_err_t voice_read_frame(const char *json, const zv_device_credential_t *credential,
                                  const char *session_id, voice_turn_t *turn)
{
    cJSON *root = cJSON_ParseWithLength(json, strlen(json));
    if (!cJSON_IsObject(root)) { cJSON_Delete(root); return ESP_ERR_INVALID_RESPONSE; }
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "v");
    const cJSON *kind = cJSON_GetObjectItemCaseSensitive(root, "type");
    const cJSON *device = cJSON_GetObjectItemCaseSensitive(root, "device_id");
    const cJSON *session = cJSON_GetObjectItemCaseSensitive(root, "session_id");
    const cJSON *correlation = cJSON_GetObjectItemCaseSensitive(root, "correlation_id");
    const cJSON *seq = cJSON_GetObjectItemCaseSensitive(root, "seq");
    unsigned count = 0;
    for (const cJSON *item = root->child; item != NULL; item = item->next) count++;
    esp_err_t result = ESP_ERR_INVALID_RESPONSE;
    if (!cJSON_IsNumber(version) || version->valuedouble != 1.0 || !cJSON_IsString(kind) ||
        !cJSON_IsString(device) || strcmp(device->valuestring, credential->device_id) != 0 ||
        !cJSON_IsString(session) || strcmp(session->valuestring, session_id) != 0 ||
        !cJSON_IsString(correlation) || strcmp(correlation->valuestring, turn->correlation_id) != 0 ||
        !cJSON_IsNumber(seq) || seq->valuedouble != (double)(turn->rx_seq + 1u)) goto done;
    turn->rx_seq++;
    if (strcmp(kind->valuestring,"conversation.ready")==0 && count==9u && turn->state==VOICE_THINKING) {
        const cJSON *conversation=cJSON_GetObjectItemCaseSensitive(root,"conversation_id");
        const cJSON *principal=cJSON_GetObjectItemCaseSensitive(root,"principal");
        const cJSON *capabilities=cJSON_GetObjectItemCaseSensitive(root,"capabilities");
        if (!cJSON_IsString(conversation) || strlen(conversation->valuestring)!=36u || !cJSON_IsString(principal) || !cJSON_IsArray(capabilities)) goto done;
        turn->state=VOICE_IDLE; result=ESP_OK;
    } else if (strcmp(kind->valuestring, "transcript.final") == 0 && count == 7u &&
        turn->state == VOICE_THINKING && !turn->transcript_seen) {
        const cJSON *text = cJSON_GetObjectItemCaseSensitive(root, "text");
        if (!cJSON_IsString(text) || strlen(text->valuestring) == 0u ||
            strlen(text->valuestring) > 4096u) goto done;
        turn->transcript_seen = true;
        result = ESP_OK;
    } else if (strcmp(kind->valuestring, "assistant.thinking") == 0 && count == 6u &&
               turn->state == VOICE_THINKING && turn->transcript_seen && !turn->thinking_seen) {
        turn->thinking_seen = true;
        turn->stage_started_ms = now_ms();
        result = ESP_OK;
    } else if (strcmp(kind->valuestring, "assistant.text") == 0 && count == 7u &&
               turn->state == VOICE_THINKING && turn->thinking_seen && !turn->text_seen) {
        const cJSON *text = cJSON_GetObjectItemCaseSensitive(root, "text");
        if (!cJSON_IsString(text) || strlen(text->valuestring) == 0u ||
            strlen(text->valuestring) > 4096u) goto done;
        turn->text_seen = true;
        turn->stage_started_ms = now_ms();
        result = ESP_OK;
    } else if (strcmp(kind->valuestring, "audio.output.start") == 0 && count == 7u &&
               turn->state == VOICE_THINKING && turn->text_seen &&
               voice_format_valid(cJSON_GetObjectItemCaseSensitive(root, "format"))) {
        result = zv_audio_voice_playback_start();
        if (result == ESP_OK) {
            turn->state = VOICE_SPEAKING;
            turn->stage_started_ms = now_ms();
            (void)zv_ui_show_voice_state(ZV_AVATAR_SPEAKING, "SPEAKING");
        }
    } else if (strcmp(kind->valuestring, "audio.output.chunk") == 0 && count == 7u &&
               turn->state == VOICE_SPEAKING) {
        const cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
        size_t bytes = 0;
        if (!cJSON_IsString(data) || strlen(data->valuestring) > 1368u ||
            mbedtls_base64_decode((unsigned char *)s_voice_output_pcm, sizeof(s_voice_output_pcm), &bytes,
                                  (const unsigned char *)data->valuestring,
                                  strlen(data->valuestring)) != 0 ||
            bytes == 0u || bytes > 1024u || (bytes & 1u) != 0u ||
            turn->output_bytes + bytes > 1920000u) goto done; /* 60 s of 16 kHz PCM16 */
        turn->output_bytes += bytes;
        result = zv_audio_voice_playback(s_voice_output_pcm, bytes / 2u);
        if (result == ESP_OK) (void)zv_ui_voice_audio(ZV_AVATAR_SPEAKING, s_voice_output_pcm, bytes / 2u);
        memset(s_voice_output_pcm, 0, sizeof(s_voice_output_pcm));
    } else if (strcmp(kind->valuestring, "audio.output.stop") == 0 && count == 6u &&
               turn->state == VOICE_SPEAKING && turn->output_bytes > 0u) {
        result = zv_audio_voice_playback_finish();
        turn->state = VOICE_IDLE;
        (void)zv_ui_show_voice_state(ZV_AVATAR_IDLE, "IDLE");
    } else if (strcmp(kind->valuestring, "error") == 0 && count == 7u &&
               turn->state != VOICE_IDLE) {
        const cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "code");
        if (!cJSON_IsString(code) || !voice_error_code_valid(code->valuestring)) goto done;
        /* Never echo gateway-controlled text into serial logs. */
        bool no_speech = strcmp(code->valuestring, "no_speech") == 0;
        if (no_speech) ESP_LOGI(TAG, "voice turn ended: category=no_speech");
        else ESP_LOGW(TAG, "voice turn failed: category=%s", voice_error_category(code->valuestring));
        zv_audio_voice_close();
        turn->state = VOICE_IDLE;
        if (no_speech)
            (void)zv_ui_show_voice_state(ZV_AVATAR_IDLE, "NO SPEECH");
        else
            (void)zv_ui_show_voice_state(ZV_AVATAR_ERROR, "ERROR");
        result = ESP_OK;
    }
done:
    cJSON_Delete(root);
    return result;
}

/* Stop publishing old-session frames before waiting for send/client locks or
 * painting a full settings screen. A pinned callback may be waiting for room
 * in the queue while holding the WebSocket lock: drain it so it can finish. */
static void voice_receive_stop(void)
{
    portENTER_CRITICAL(&s_voice_queue_lock);
    s_voice_active = false;
    s_voice_discarding = true;
    portEXIT_CRITICAL(&s_voice_queue_lock);
    for (;;) {
        portENTER_CRITICAL(&s_voice_queue_lock);
        uint32_t callbacks = s_voice_queue_callbacks;
        portEXIT_CRITICAL(&s_voice_queue_lock);
        if (callbacks == 0u) break;
        (void)xQueueReceive(s_voice_queue, &s_voice_processing, 0);
        vTaskDelay(1);
    }
}

static void voice_cancel_turn(esp_websocket_client_handle_t client,
                              const zv_device_credential_t *credential,
                              const char *session_id, voice_turn_t *turn)
{
    zv_audio_voice_close();
    turn->state = VOICE_IDLE;
    voice_receive_stop();
    /* Closing this WebSocket also cancels the server worker. A failed courtesy
     * notification must not prevent local controls or misclassify user cancel.
     * Actual failure to establish the next session still uses normal backoff. */
    if (voice_send_event(client, credential, session_id, turn, "cancel", false) != ESP_OK) {
        ESP_LOGW(TAG, "cancel notification unavailable; closing old session");
    }
}

static esp_err_t voice_session_loop(esp_websocket_client_handle_t client,
                                    const zv_device_credential_t *credential,
                                    const char *session_id, bool *reconnect)
{
    voice_turn_t turn = {0};
    /* A contact held through reauthentication must never start a new turn. */
    bool touch_latched = true;
    uint32_t mic_pressed_ms = UINT32_MAX;
    if (zv_ui_settings_active()) (void)zv_ui_settings_resume(wifi_signal_bars(), true);
    else (void)zv_ui_show_voice_state(ZV_AVATAR_IDLE, "IDLE");
    s_voice_queue_storage = heap_caps_malloc(VOICE_QUEUE_DEPTH * sizeof(voice_frame_t),
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_voice_queue_storage == NULL) return ESP_ERR_NO_MEM;
    s_voice_queue = xQueueCreateStatic(VOICE_QUEUE_DEPTH, sizeof(voice_frame_t),
                                       s_voice_queue_storage, &s_voice_queue_control);
    if (s_voice_queue == NULL) {
        heap_caps_free(s_voice_queue_storage);
        s_voice_queue_storage = NULL;
        return ESP_ERR_NO_MEM;
    }
    portENTER_CRITICAL(&s_voice_queue_lock);
    s_voice_active = true;
    portEXIT_CRITICAL(&s_voice_queue_lock);
    esp_err_t err = ESP_OK;
    uint32_t last_header_ms = 0u;
    while (err == ESP_OK) {
        EventBits_t bits = xEventGroupGetBits(s_events);
        if ((bits & (WS_FAIL_BIT | WIFI_DISCONNECTED_BIT)) != 0u) {
            ESP_LOGW(TAG, "voice session failed: category=transport");
            err = ESP_ERR_INVALID_RESPONSE;
            break;
        }
        bool touched = false;
        uint16_t x = 0, y = 0;
        err = zv_board_touch_poll(&touched, &x, &y);
        if (err != ESP_OK) break;
        err = zv_power_update(now_ms(), touched, turn.state != VOICE_IDLE);
        if (err != ESP_OK) break;
        if (zv_ui_settings_active()) {
            if ((uint32_t)(now_ms() - last_header_ms) >= 1000u) {
                last_header_ms = now_ms();
                err = zv_ui_settings_resume(wifi_signal_bars(), true);
                if (err != ESP_OK) break;
            }
            err = zv_ui_settings_poll(touched, x, y, now_ms());
            if (err != ESP_OK) break;
            touch_latched = true; mic_pressed_ms = UINT32_MAX;
            if (!zv_ui_settings_active()) (void)zv_ui_show_voice_state(ZV_AVATAR_IDLE, "IDLE");
            vTaskDelay(pdMS_TO_TICKS(16u));
            continue;
        }
        if (turn.state==VOICE_IDLE && zv_ui_take_conversation_reset()) {
            rand_uuid(turn.correlation_id); turn.started_ms=now_ms(); turn.stage_started_ms=turn.started_ms;
            err=voice_send_event(client,credential,session_id,&turn,"conversation.reset",false);
            if (err!=ESP_OK) break;
            turn.state=VOICE_THINKING; (void)zv_ui_show_voice_state(ZV_AVATAR_THINKING,"NEW CONVERSATION");
        }
        bool on_mic = touched && zv_ui_touch_is_voice_function(x, y);
        bool on_volume = touched && zv_ui_touch_is_volume(x, y);
        /* Reference UI: menu works in every voice state; active audio is cancelled first. */
        if (touched && !touch_latched && (zv_ui_touch_is_home(x, y) || on_volume)) {
            ESP_LOGI(TAG, "voice control selected: %s", on_volume ? "volume" : "menu");
            bool active_turn = turn.state != VOICE_IDLE;
            if (turn.state != VOICE_IDLE) {
                voice_cancel_turn(client, credential, session_id, &turn);
            }
            touch_latched = true;
            mic_pressed_ms = UINT32_MAX;
            err = on_volume ? zv_ui_settings_open_volume(wifi_signal_bars(), !active_turn) :
                              zv_ui_settings_open(wifi_signal_bars(), !active_turn);
            if (err != ESP_OK) break;
            /* Reconnect after cancellation; queued frames belong to the old turn. */
            if (active_turn) { *reconnect = true; break; }
            continue;
        }
        /* Explicit cancel also accepts a slide from the held recording face.
         * Close before sending: network latency must not prolong capture/audio.
         * Reauthenticate to discard any queued output from the cancelled turn. */
        if (turn.state != VOICE_IDLE && touched && zv_ui_cancel_hit(x, y) &&
            (turn.state == VOICE_LISTENING || !touch_latched)) {
            voice_cancel_turn(client, credential, session_id, &turn);
            touch_latched = true;
            (void)show_link_state(ZV_LINK_RECONNECTING, 0u);
            *reconnect = true;
            break;
        }
        if (turn.state != VOICE_IDLE && turn.state != VOICE_LISTENING &&
            touched && !touch_latched && on_mic) {
            voice_cancel_turn(client, credential, session_id, &turn);
            touch_latched = true;
            (void)show_link_state(ZV_LINK_RECONNECTING, 0u);
            *reconnect = true;
            break;
        }
        if (turn.state == VOICE_THINKING) (void)zv_ui_voice_tick();
        /* Handle local controls before a queued frame can block on codec/LCD I/O. */
        bool frame_received = xQueueReceive(s_voice_queue, &s_voice_processing, 0) == pdTRUE;
        if (frame_received) {
            if (turn.state == VOICE_IDLE ||
                voice_read_frame(s_voice_processing.json, credential, session_id, &turn) != ESP_OK) {
                ESP_LOGW(TAG, "voice session failed: category=frame_invalid");
                err = ESP_ERR_INVALID_RESPONSE;
                break;
            }
            if (turn.state == VOICE_IDLE) mic_pressed_ms = UINT32_MAX;
        }
        if (turn.state == VOICE_IDLE) {
            if (on_mic) {
                if (!touch_latched) { touch_latched = true; mic_pressed_ms = now_ms(); }
                if (mic_pressed_ms != UINT32_MAX &&
                    (uint32_t)(now_ms() - mic_pressed_ms) >= 200u) {
                    err = zv_power_update(now_ms(), true, true);
                    if (err == ESP_OK) err = zv_audio_voice_set_volume(zv_ui_voice_volume());
                    if (err == ESP_OK) err = zv_audio_voice_open();
                    if (err != ESP_OK) break;
                    rand_uuid(turn.correlation_id);
                    turn.started_ms = now_ms();
                    turn.stage_started_ms = turn.started_ms;
                    turn.input_bytes = 0u;
                    turn.output_bytes = 0u;
                    turn.transcript_seen = turn.thinking_seen = turn.text_seen = false;
                    err = voice_send_event(client, credential, session_id, &turn,
                                           "audio.input.start", true);
                    if (err != ESP_OK) break;
                    turn.state = VOICE_LISTENING;
                    (void)zv_ui_show_voice_state(ZV_AVATAR_LISTENING, "LISTENING");
                }
            } else if (!touched) {
                touch_latched = false;
                mic_pressed_ms = 0u;
            }
        } else if (turn.state == VOICE_LISTENING) {
            if (touched && (uint32_t)(now_ms() - turn.started_ms) < 15000u &&
                turn.input_bytes + 512u <= 480000u) {
                err = zv_audio_voice_capture(s_voice_input_pcm);
                if (err == ESP_OK) err = voice_send_chunk(client, credential, session_id, &turn, s_voice_input_pcm);
                if (err == ESP_OK) (void)zv_ui_voice_audio(ZV_AVATAR_LISTENING, s_voice_input_pcm, 256u);
                memset(s_voice_input_pcm, 0, sizeof(s_voice_input_pcm));
                if (err != ESP_OK) break;
                turn.input_bytes += 512u;
            } else {
                err = voice_send_event(client, credential, session_id, &turn,
                                       turn.input_bytes > 0u ? "audio.input.stop" : "cancel", false);
                if (err != ESP_OK) break;
                if (turn.input_bytes > 0u) {
                    turn.state = VOICE_THINKING;
                    turn.stage_started_ms = now_ms();
                    (void)zv_ui_show_voice_state(ZV_AVATAR_THINKING, "THINKING");
                } else {
                    zv_audio_voice_close();
                    turn.state = VOICE_IDLE;
                    (void)show_link_state(ZV_LINK_RECONNECTING, 0u);
                    *reconnect = true;
                    break;
                }
            }
        } else if (!touched) {
            touch_latched = false;
        }
        if (voice_turn_expired(&turn, now_ms())) {
            ESP_LOGW(TAG, "voice turn failed: category=client_deadline");
            (void)voice_send_event(client, credential, session_id, &turn, "cancel", false);
            err = ESP_ERR_TIMEOUT;
            break;
        }
        if ((turn.state == VOICE_IDLE || turn.state == VOICE_THINKING) &&
            (uint32_t)(now_ms() - last_header_ms) >= 1000u) {
            last_header_ms = now_ms();
            (void)zv_ui_voice_refresh(wifi_signal_bars());
        }
        /* The codec write already paces output. Extra sleeps make the consumer
         * slower than the gateway's 16 kHz stream and overflow the small queue. */
        err = zv_power_update(now_ms(), touched, turn.state != VOICE_IDLE);
        if (err != ESP_OK) break;
        if (!frame_received && turn.state != VOICE_LISTENING) {
#if CONFIG_ZV_VOICE_POWER_SAVING
            if (turn.state == VOICE_IDLE && !touched) zv_board_touch_wait(100u);
            else vTaskDelay(pdMS_TO_TICKS(16u));
#else
            vTaskDelay(pdMS_TO_TICKS(16u));
#endif
        }
    }
    voice_receive_stop();
    zv_audio_voice_close();
    (void)zv_power_update(now_ms(), false, false);
    memset(s_voice_queue_storage, 0, VOICE_QUEUE_DEPTH * sizeof(voice_frame_t));
    memset(&s_voice_incoming, 0, sizeof(s_voice_incoming));
    memset(&s_voice_processing, 0, sizeof(s_voice_processing));
    vQueueDelete(s_voice_queue);
    s_voice_queue = NULL;
    heap_caps_free(s_voice_queue_storage);
    s_voice_queue_storage = NULL;
    return err;
}
#endif

static esp_err_t websocket_attempt(const char *uri, const char *hello, const char *hello_id,
                                   const zv_device_credential_t *credential,
                                   zv_link_supervisor_t *supervisor, bool hold_session,
                                   uint32_t deadline_ms, bool *reconnect)
{
    *reconnect = false;
    esp_websocket_client_config_t cfg = {.uri = uri, .subprotocol = ZV_PROTOCOL_SUBPROTOCOL,
        .buffer_size = ZV_VOICE_ENABLED ? 4096 : 1024, .disable_auto_reconnect = true};
#if CONFIG_ZV_M2_LAB_CA
    extern const uint8_t lab_ca_pem_start[] asm("_binary_lab_ca_pem_start");
    extern const uint8_t lab_ca_pem_end[] asm("_binary_lab_ca_pem_end");
    ESP_RETURN_ON_FALSE((size_t)(&lab_ca_pem_end[0] - &lab_ca_pem_start[0]) > 0u,
                        ESP_ERR_INVALID_SIZE, TAG, "embedded lab CA empty");
    cfg.cert_pem = (const char *)lab_ca_pem_start;
    ESP_LOGW(TAG, "using embedded lab CA; hostname verification enabled");
#else
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
#endif
#if CONFIG_ZV_M3A_VOICE_MODE
    portENTER_CRITICAL(&s_voice_queue_lock);
    s_voice_discarding = false;
    portEXIT_CRITICAL(&s_voice_queue_lock);
#endif
    xEventGroupClearBits(s_events, WS_CONNECTED_BIT | WS_TEXT_BIT | WS_FAIL_BIT);
    s_rx[0] = '\0';
    esp_websocket_client_handle_t client = esp_websocket_client_init(&cfg);
    ESP_RETURN_ON_FALSE(client != NULL, ESP_ERR_NO_MEM, TAG, "WebSocket init failed");
    esp_err_t err = esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, ws_event_handler, NULL);
    if (err == ESP_OK) err = esp_websocket_client_start(client);
    EventBits_t bits = 0u;
    if (err == ESP_OK) {
        bits = xEventGroupWaitBits(s_events, WS_CONNECTED_BIT | WS_FAIL_BIT, pdTRUE, pdFALSE, pdMS_TO_TICKS(WS_TIMEOUT_MS));
        err = (bits & WS_CONNECTED_BIT) != 0u && (bits & WS_FAIL_BIT) == 0u ? ESP_OK : ESP_ERR_TIMEOUT;
    }
    if (err == ESP_OK) {
        int sent = esp_websocket_client_send_text(client, hello, (int)strlen(hello), pdMS_TO_TICKS(5000));
        err = sent == (int)strlen(hello) ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
        if (err == ESP_OK) ESP_LOGI(TAG, "device.hello sent (%d bytes)", sent);
    }
    if (err == ESP_OK) {
        bits = xEventGroupWaitBits(s_events, WS_TEXT_BIT | WS_FAIL_BIT, pdTRUE, pdFALSE, pdMS_TO_TICKS(WS_TIMEOUT_MS));
        err = (bits & WS_TEXT_BIT) != 0u && (bits & WS_FAIL_BIT) == 0u ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "gateway challenge received (%u bytes; payload redacted)", (unsigned)strlen(s_rx));
        err = credential == NULL ? ESP_ERR_INVALID_STATE :
              build_auth_proof(s_rx, hello_id, credential, s_tx, sizeof(s_tx));
    }
    if (err == ESP_OK) {
        xEventGroupClearBits(s_events, WS_TEXT_BIT | WS_FAIL_BIT);
        int sent = esp_websocket_client_send_text(client, s_tx, (int)strlen(s_tx), pdMS_TO_TICKS(5000));
        memset(s_tx, 0, sizeof(s_tx));
        err = sent > 0 ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
        if (err == ESP_OK) ESP_LOGI(TAG, "device credential proof sent (%d bytes; proof redacted)", sent);
    }
    if (err == ESP_OK) {
        bits = xEventGroupWaitBits(s_events, WS_TEXT_BIT | WS_FAIL_BIT, pdTRUE, pdFALSE,
                                   pdMS_TO_TICKS(WS_TIMEOUT_MS));
        err = (bits & WS_TEXT_BIT) != 0u && (bits & WS_FAIL_BIT) == 0u &&
                      session_accepted(s_rx, hello_id, credential)
                  ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "authenticated gateway session accepted; identifiers redacted");
        ESP_LOGI(TAG, "link_diag event=authenticated uptime_ms=%llu",
                 (unsigned long long)(esp_timer_get_time() / 1000));
        err = show_link_state(ZV_LINK_ONLINE, 0u);
        if (err == ESP_OK && zv_link_on_connected(supervisor) != ZV_OK) err = ESP_ERR_INVALID_STATE;
#if CONFIG_ZV_M3A_VOICE_MODE
        char session_id[40];
        if (!json_string(s_rx, "session_id", session_id, sizeof(session_id))) {
            err = ESP_ERR_INVALID_RESPONSE;
        } else {
            err = voice_session_loop(client, credential, session_id, reconnect);
        }
#else
        while (err == ESP_OK && hold_session && (int32_t)(deadline_ms - now_ms()) > 0) {
            sample_memory(false);
            bits = xEventGroupWaitBits(s_events, WS_FAIL_BIT | WIFI_DISCONNECTED_BIT, pdTRUE, pdFALSE, pdMS_TO_TICKS(POLL_MS));
            if ((bits & (WS_FAIL_BIT | WIFI_DISCONNECTED_BIT)) != 0u) { err = ESP_ERR_INVALID_RESPONSE; break; }
        }
#endif
    }
    (void)esp_websocket_client_stop(client); (void)esp_websocket_client_destroy(client);
    return err;
}

esp_err_t zv_wss_acceptance_demo(void)
{
    static zv_cred_store_t store; static char hello[ZV_HELLO_JSON_MAX]; static char uuid[37];
    char public_credential[160], uri[160]; zv_gateway_endpoint_t endpoint; zv_link_supervisor_t supervisor;
#ifdef CONFIG_ZV_M2_LINK_SOAK_MODE
    const bool soak = true;
    const uint32_t budget_ms = (uint32_t)CONFIG_ZV_M2_LINK_SOAK_DURATION_MS;
#else
    const bool soak = false;
    const uint32_t budget_ms = LINK_BUDGET_MS;
#endif
    uint32_t successes = 0u, failures = 0u;
    uint32_t last_ui_ms = 0u;
    zv_link_state_t last_ui_state = ZV_LINK_ONLINE; /* force initial offline paint */
    ESP_LOGW(TAG, "M2 WSS mode: supervised fail-closed link; gateway actions locked");
    memset(&s_acceptance, 0, sizeof(s_acceptance));
#if CONFIG_ZV_M3A_VOICE_MODE && !CONFIG_ZV_VOICE_STARTUP_DIAGNOSTICS
    ESP_RETURN_ON_ERROR(zv_board_voice_ui_init(), TAG, "voice UI initialization failed");
#else
    ESP_RETURN_ON_ERROR(zv_board_m0_acceptance_demo(), TAG, "M0 challenge failed under WSS mode");
#endif
    s_events = xEventGroupCreate(); ESP_RETURN_ON_FALSE(s_events != NULL, ESP_ERR_NO_MEM, TAG, "event group failed");
    ESP_RETURN_ON_FALSE(zv_gateway_parse_origin(CONFIG_ZV_M2_GATEWAY_ORIGIN, &endpoint) == ZV_OK,
                        ESP_ERR_INVALID_ARG, TAG, "gateway origin rejected");
    ESP_RETURN_ON_ERROR(zv_storage_init(), TAG, "NVS init failed"); ESP_RETURN_ON_ERROR(zv_storage_load(&store), TAG, "credential load failed");
    const zv_device_credential_t *credential = zv_cred_store_current(&store);
    if (credential != NULL && zv_cred_format_public(credential, public_credential, sizeof(public_credential)) == ZV_OK)
        ESP_LOGI(TAG, "using gateway credential: %s", public_credential);
    else ESP_LOGE(TAG, "no gateway credential; authenticated session is fail-closed");
    ESP_RETURN_ON_FALSE(credential != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "gateway credential required");
    /* The credential is enrolled for one origin; dialling another can only fail, and on
     * 2026-10-14 a lab overlay sent the puck to a dead address for an evening. Origins are
     * public (both are already logged above), so name both and refuse. */
    if (!zv_gateway_origin_matches(CONFIG_ZV_M2_GATEWAY_ORIGIN, credential->gateway_origin)) {
        ESP_LOGE(TAG, "gateway origin mismatch: build=%s credential=%s; refusing to connect "
                 "(set CONFIG_ZV_M2_GATEWAY_ORIGIN to the credential origin)",
                 CONFIG_ZV_M2_GATEWAY_ORIGIN, credential->gateway_origin);
        return ESP_ERR_INVALID_STATE;
    }
    rand_uuid(uuid); ESP_RETURN_ON_FALSE(zv_hello_build(hello, sizeof(hello), uuid, credential->device_id, "0.1.0") == ZV_OK, ESP_FAIL, TAG, "hello build failed");
    int n = snprintf(uri, sizeof(uri), "wss://%s:%u%s", endpoint.host, (unsigned)endpoint.port, endpoint.path);
    ESP_RETURN_ON_FALSE(n > 0 && (size_t)n < sizeof(uri), ESP_ERR_INVALID_SIZE, TAG, "gateway URI too long");
    zv_link_config_t cfg = {.backoff_base_ms = 1000u, .backoff_max_ms = 30000u,
        .breaker = {.failure_threshold = 5u, .open_duration_ms = 60000u, .half_open_max_trials = 1u},
        .degraded_after_failures = 3u};
    ESP_RETURN_ON_FALSE(zv_link_init(&supervisor, &cfg, link_random, NULL, now_ms()) == ZV_OK, ESP_ERR_INVALID_STATE, TAG, "supervisor init failed");
    /*
     * Boot screen: panel initialization succeeded and nothing has joined yet, so
     * show Zenti waking for a beat before the link loop owns the display.
     * Presentation only; a missing UI never blocks the gated link flow.
     */
    if (zv_ui_show_booting() == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(BOOT_SCREEN_HOLD_MS));
    }
    ESP_RETURN_ON_ERROR(wifi_start(), TAG, "Wi-Fi start failed");
    ESP_RETURN_ON_ERROR(zv_power_init(now_ms()), TAG, "voice power policy init failed");
    /*
     * Do not baseline heap before Wi-Fi/TLS allocate: the settle looks like a
     * drop past max_internal_drop and poisons memory_ok for the whole soak.
     * The first post-attempt sample becomes the baseline instead.
     */
    const uint32_t deadline = now_ms() + budget_ms;
    while (ZV_VOICE_ENABLED || (int32_t)(deadline - now_ms()) > 0) {
        /* After cancelling into settings, finish local interaction before any
         * blocking Wi-Fi/TLS/auth attempt. The old turn is already discarded.
         * Preserve the selected screen and poll release/drag even while offline;
         * closing settings resumes the existing supervisor/backoff policy. */
        if (zv_ui_settings_active()) {
            bool touched = false;
            uint16_t x = 0, y = 0;
            if (last_ui_ms == 0u || (uint32_t)(now_ms() - last_ui_ms) >= 1000u) {
                ESP_RETURN_ON_ERROR(zv_ui_settings_resume(wifi_signal_bars(), false), TAG,
                                    "offline settings status failed");
                last_ui_ms = now_ms();
            }
            ESP_RETURN_ON_ERROR(zv_board_touch_poll(&touched, &x, &y), TAG,
                                "offline settings touch failed");
            ESP_RETURN_ON_ERROR(zv_power_update(now_ms(), touched, false), TAG,
                                "offline settings power failed");
            ESP_RETURN_ON_ERROR(zv_ui_settings_poll(touched, x, y, now_ms()), TAG,
                                "offline settings poll failed");
            if (!zv_ui_settings_active()) last_ui_ms = 0u;
            vTaskDelay(pdMS_TO_TICKS(16u));
            continue;
        }
        bool attempt = false; ESP_RETURN_ON_FALSE(zv_link_poll(&supervisor, now_ms(), &attempt) == ZV_OK, ESP_ERR_INVALID_STATE, TAG, "supervisor poll failed");
        if (!attempt) {
            uint32_t elapsed = now_ms() - supervisor.wait_start_ms;
            uint32_t retry = elapsed < supervisor.wait_ms ? supervisor.wait_ms - elapsed : 0u;
            zv_link_state_t state = zv_link_state(&supervisor);
            bool touched = false;
            (void)zv_board_touch_poll(&touched, NULL, NULL);
            (void)zv_power_update(now_ms(), touched, false);
            if (state != last_ui_state || (uint32_t)(now_ms() - last_ui_ms) >= 1000u) {
                (void)show_link_state(state, retry);
                last_ui_state = state;
                last_ui_ms = now_ms();
            }
            vTaskDelay(pdMS_TO_TICKS(POLL_MS)); continue;
        }
        ESP_RETURN_ON_ERROR(zv_power_update(now_ms(), false, true), TAG, "connection power boost failed");
        (void)show_link_state(zv_link_state(&supervisor), 0u);
        bool reconnect = false;
        esp_err_t err = wifi_connect_attempt();
        if (err == ESP_OK) {
#if CONFIG_ZV_M3A_VOICE_MODE
            rand_uuid(uuid);
            if (zv_hello_build(hello, sizeof(hello), uuid, credential->device_id, "0.1.0") != ZV_OK)
                return ESP_ERR_INVALID_SIZE;
#endif
            err = websocket_attempt(uri, hello, uuid, credential, &supervisor,
                                    ZV_VOICE_ENABLED || soak, deadline, &reconnect);
            if (zv_link_state(&supervisor) == ZV_LINK_ONLINE) {
                successes++;
                if (!soak && !ZV_VOICE_ENABLED && s_acceptance.ui.failure_count == 0u) {
                    ESP_LOGI(TAG, "M2 WSS acceptance passed (authenticated session); platform remains gated");
                    return ESP_OK;
                }
            }
        }
        if (reconnect && err == ESP_OK) {
            ESP_RETURN_ON_FALSE(zv_link_on_reconnect(&supervisor, now_ms()) == ZV_OK,
                                ESP_ERR_INVALID_STATE, TAG, "planned reconnect state failed");
            ESP_LOGI(TAG, "cancel complete; refreshing authenticated session");
            if (zv_ui_settings_active()) (void)zv_ui_settings_resume(wifi_signal_bars(), false);
        }
        if (err != ESP_OK) {
            failures++; (void)zv_link_on_failure(&supervisor, now_ms());
            ESP_LOGW(TAG, "link failure=%lu state=%s retry_ms=%lu breaker_trips=%lu error=%s; secrets redacted",
                (unsigned long)failures, zv_link_state_name(zv_link_state(&supervisor)), (unsigned long)supervisor.wait_ms,
                (unsigned long)supervisor.breaker.trip_count, esp_err_to_name(err));
        }
        /*
         * Baseline heap only after the first accepted session. Sampling earlier
         * (pre-TLS / pre-Wi-Fi) treats the allocator settle as a drop past
         * max_internal_drop and poisons memory_ok for the whole soak.
         */
        if (successes > 0u) {
            sample_memory(false);
        }
    }
    sample_memory(true);
    const bool memory_complete = s_acceptance.memory_samples >= 2u && s_acceptance.memory_ok &&
                                 zv_link_memory_stable(&s_acceptance.baseline, &s_acceptance.last,
                                                       &s_memory_limits);
    const bool ui_complete = zv_link_ui_complete(&s_acceptance.ui);
    if (soak && successes >= 2u && failures >= 5u && supervisor.breaker.trip_count >= 1u &&
        memory_complete && ui_complete) {
        ESP_LOGI(TAG, "M2 link fault soak passed sessions=%lu failures=%lu breaker_trips=%lu; platform remains gated",
            (unsigned long)successes, (unsigned long)failures, (unsigned long)supervisor.breaker.trip_count); return ESP_OK;
    }
    ESP_LOGE(TAG, "M2 link acceptance incomplete sessions=%lu failures=%lu breaker_trips=%lu memory_samples=%lu memory_ok=%s ui_mask=0x%lx ui_failures=%lu",
        (unsigned long)successes, (unsigned long)failures, (unsigned long)supervisor.breaker.trip_count,
        (unsigned long)s_acceptance.memory_samples, memory_complete ? "yes" : "no",
        (unsigned long)s_acceptance.ui.rendered_mask, (unsigned long)s_acceptance.ui.failure_count);
    return ESP_ERR_TIMEOUT;
}
#else
esp_err_t zv_wss_acceptance_demo(void) { return ESP_ERR_NOT_SUPPORTED; }
#endif
