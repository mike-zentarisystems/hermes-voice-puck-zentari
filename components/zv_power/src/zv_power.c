/* SPDX-License-Identifier: Apache-2.0 */
#include "zv_power.h"
#include "sdkconfig.h"
#if CONFIG_ZV_VOICE_POWER_SAVING
#include "esp_check.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_wifi.h"
#include "zv_board.h"
static const char *TAG = "zv_power";
static esp_pm_lock_handle_t s_cpu_lock;
static bool s_ready, s_busy, s_dimmed;
static uint32_t s_activity_ms;

esp_err_t zv_power_init(uint32_t now_ms)
{
    if (s_ready) return ESP_OK;
    const esp_pm_config_t cfg = {.max_freq_mhz=240, .min_freq_mhz=80,
                                 .light_sleep_enable=false};
    ESP_RETURN_ON_ERROR(esp_pm_configure(&cfg), TAG, "CPU power configuration failed");
    ESP_RETURN_ON_ERROR(esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "voice", &s_cpu_lock),
                        TAG, "voice CPU lock failed");
    esp_err_t err = esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    if (err == ESP_OK) err = zv_board_backlight_set(CONFIG_ZV_VOICE_AWAKE_BRIGHTNESS);
    if (err != ESP_OK) {
        esp_pm_lock_delete(s_cpu_lock);
        s_cpu_lock = NULL;
        return err;
    }
    s_activity_ms = now_ms;
    s_ready = true;
    ESP_LOGI(TAG, "power policy ready: idle CPU 80 MHz, active 240 MHz, automatic light sleep off");
    return ESP_OK;
}

esp_err_t zv_power_update(uint32_t now_ms, bool touched, bool voice_active)
{
    if (!s_ready) return ESP_OK;
    if (touched || voice_active) s_activity_ms = now_ms;
    if (voice_active != s_busy) {
        if (voice_active) {
            ESP_RETURN_ON_ERROR(esp_pm_lock_acquire(s_cpu_lock), TAG, "voice CPU boost failed");
            esp_err_t err = esp_wifi_set_ps(WIFI_PS_NONE);
            if (err != ESP_OK) { esp_pm_lock_release(s_cpu_lock); return err; }
        } else {
            ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_MIN_MODEM), TAG, "idle Wi-Fi power save failed");
            ESP_RETURN_ON_ERROR(esp_pm_lock_release(s_cpu_lock), TAG, "voice CPU release failed");
        }
        s_busy = voice_active;
        ESP_LOGI(TAG, "power phase=%s", s_busy ? "active" : "idle");
    }
    bool dim = !voice_active && (uint32_t)(now_ms-s_activity_ms) >=
               (uint32_t)CONFIG_ZV_VOICE_DIM_AFTER_SECONDS * 1000u;
    if (dim != s_dimmed) {
        ESP_RETURN_ON_ERROR(zv_board_backlight_set(dim ? CONFIG_ZV_VOICE_DIM_BRIGHTNESS :
                                                  CONFIG_ZV_VOICE_AWAKE_BRIGHTNESS), TAG,
                            "backlight power transition failed");
        s_dimmed = dim;
        ESP_LOGI(TAG, "backlight phase=%s", dim ? "dim" : "awake");
    }
    return ESP_OK;
}
#else
esp_err_t zv_power_init(uint32_t now_ms) { (void)now_ms; return ESP_OK; }
esp_err_t zv_power_update(uint32_t now_ms, bool touched, bool voice_active)
{ (void)now_ms; (void)touched; (void)voice_active; return ESP_OK; }
#endif
