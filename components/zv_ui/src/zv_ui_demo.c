/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 */
#include "zv_ui.h"

#if CONFIG_ZV_UI_ACCEPTANCE_MODE || CONFIG_ZV_M2_WSS_MODE

#include <inttypes.h>
#include <stdio.h>
#include "esp_timer.h"
#include <string.h>
#include <time.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "zv_avatar_model.h"
#include "zv_board.h"
#include "zv_lipsync.h"
#include "zv_status.h"
#include "zv_ui_draw.h"

static const char *TAG = "zv_ui";

#define UI_STATE_HOLD_MS 2500u
#define UI_TOUCH_POLL_MS 50u
#define UI_CONFIRM_TIMEOUT_MS 30000u
#define UI_FRAME_MS 50u
#define UI_AMP_HZ 2

/** Board I/O uses esp_err_t; the portable painter canvas uses zv_status_t. */
static zv_status_t esp_to_zv(esp_err_t err)
{
    if (err == ESP_OK) {
        return ZV_OK;
    }
    if (err == ESP_ERR_INVALID_ARG) {
        return ZV_ERR_INVALID_ARG;
    }
    if (err == ESP_ERR_INVALID_STATE) {
        return ZV_ERR_INVALID_STATE;
    }
    /* Timeout, no-mem, and other LCD/DMA failures are board I/O, not a
       portable-core condition. */
    return ZV_ERR_HARDWARE_GATE;
}

/** Portable-core APIs return zv_status_t; ESP_RETURN_ON_ERROR wants esp_err_t. */
static esp_err_t zv_to_esp(zv_status_t status)
{
    if (status == ZV_OK) {
        return ESP_OK;
    }
    if (status == ZV_ERR_INVALID_ARG) {
        return ESP_ERR_INVALID_ARG;
    }
    if (status == ZV_ERR_INVALID_STATE) {
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_FAIL;
}

static zv_status_t canvas_fill(void *ctx, int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color)
{
    (void)ctx;
    return esp_to_zv(zv_board_lcd_fill_rect((uint16_t)x0, (uint16_t)y0, (uint16_t)x1, (uint16_t)y1, color));
}

static zv_status_t canvas_row(void *ctx, int16_t x, int16_t y, uint16_t width, const uint16_t *pixels)
{
    (void)ctx;
    return esp_to_zv(zv_board_lcd_draw_row((uint16_t)x, (uint16_t)y, width, pixels));
}

/** Use synchronized system time first, then the V2 PCF85063 calendar. */
static bool format_datetime(char *date_buf, size_t date_len, char *clock_buf, size_t clock_len)
{
    static const char *const k_days[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
    static const char *const k_months[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                          "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
    if (date_buf == NULL || clock_buf == NULL || date_len < 16u || clock_len < 12u) return false;
    time_t now = time(NULL);
    struct tm tmv;
    bool valid = now >= 1577836800 && localtime_r(&now, &tmv) != NULL;
    if (!valid && zv_board_rtc_read(&tmv) == ESP_OK &&
        tmv.tm_year >= 120 && tmv.tm_year <= 199 &&
        tmv.tm_mon >= 0 && tmv.tm_mon <= 11 && tmv.tm_mday >= 1 && tmv.tm_mday <= 31) valid = true;
    if (!valid || tmv.tm_wday < 0 || tmv.tm_wday > 6) {
        (void)snprintf(date_buf, date_len, "DATE NOT SET");
        clock_buf[0] = '\0';
        return false;
    }
    const int hour12 = tmv.tm_hour % 12 == 0 ? 12 : tmv.tm_hour % 12;
    (void)snprintf(date_buf, date_len, "%s, %s %d", k_days[tmv.tm_wday], k_months[tmv.tm_mon], tmv.tm_mday);
    (void)snprintf(clock_buf, clock_len, "%d:%02d %s", hour12, tmv.tm_min, tmv.tm_hour < 12 ? "AM" : "PM");
    return true;
}

/** Map LiPo mV into 0..100. Unknown/not-ready leaves battery_ok false. */
static void fill_battery_chrome(zv_ui_chrome_t *chrome)
{
    uint32_t mv = 0;
    if (zv_board_battery_read_mv(&mv) != ESP_OK) {
        chrome->battery_ok = false;
        chrome->battery_percent = 0u;
        return;
    }
    chrome->battery_ok = true;
    if (mv <= 3300u) {
        chrome->battery_percent = 0u;
    } else if (mv >= 4200u) {
        chrome->battery_percent = 100u;
    } else {
        chrome->battery_percent = (uint8_t)(((mv - 3300u) * 100u) / 900u);
    }
}

static void fill_status_chrome(zv_ui_chrome_t *chrome, char *date_buf, size_t date_len,
                               char *clock_buf, size_t clock_len)
{
    chrome->show_mic = false;
    chrome->show_volume = true;
    chrome->volume = zv_ui_voice_volume_state();
    (void)format_datetime(date_buf, date_len, clock_buf, clock_len);
    chrome->date_text = date_buf;
    chrome->clock_text = clock_buf[0] != '\0' ? clock_buf : "TIME NOT SET";
    chrome->show_connectivity = true;
    fill_battery_chrome(chrome);
}

/** Shared status-surface state: the boot screen and link screens are one UI. */
static zv_avatar_model_t s_status_model;
static bool s_status_ui_ready;
static uint8_t s_last_link_bars;
static zv_avatar_state_t s_voice_state = ZV_AVATAR_IDLE;
static uint32_t s_voice_refresh_ms, s_voice_audio_ms;
static uint32_t s_voice_tick_ms;
static uint8_t s_voice_thinking_phase = UINT8_MAX;
static zv_envelope_t s_voice_envelope;

static zv_envelope_config_t voice_envelope_config(zv_avatar_state_t state)
{
    zv_envelope_config_t config = zv_envelope_default_config();
    if (state == ZV_AVATAR_LISTENING) {
        /* Microphone PCM is much quieter than speaker PCM. This changes only
         * the visual meter, never captured/transmitted samples or STT gates. */
        config.noise_floor_rms = 80u;
        config.full_scale_rms = 2000u;
    }
    return config;
}

/* Cache visible header content, not elapsed time: RTC/SNTP corrections still
 * appear promptly, while ADC fluctuations never erase the clock. */
static struct {
    bool valid, battery_ok;
    uint8_t battery_percent, signal_bars, pending_battery, pending_samples;
    char date[16], clock[16];
} s_voice_header;

static void remember_voice_header(const zv_ui_chrome_t *chrome)
{
    s_voice_header.valid = true;
    s_voice_header.battery_ok = chrome->battery_ok;
    s_voice_header.battery_percent = chrome->battery_percent;
    s_voice_header.signal_bars = chrome->signal_bars;
    snprintf(s_voice_header.date, sizeof(s_voice_header.date), "%s", chrome->date_text);
    snprintf(s_voice_header.clock, sizeof(s_voice_header.clock), "%s", chrome->clock_text);
}


/** Panel + avatar are proven lazily so the first status screen never races M0. */
static esp_err_t ensure_status_ui_ready(void)
{
    if (s_status_ui_ready) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(zv_board_lcd_init(), TAG, "status UI LCD init failed");
    ESP_RETURN_ON_ERROR(zv_board_backlight_set(35), TAG, "status UI backlight failed");
    ESP_RETURN_ON_ERROR(zv_board_battery_init(), TAG, "battery ADC init failed");
    ESP_RETURN_ON_ERROR(zv_to_esp(zv_avatar_init(&s_status_model, 0u, 0x4c494e4bu)), TAG,
                        "status UI avatar init failed");
    zv_ui_settings_init();
    zv_envelope_config_t envelope_config = zv_envelope_default_config();
    (void)zv_envelope_init(&s_voice_envelope, &envelope_config);
    s_status_ui_ready = true;
    return ESP_OK;
}

esp_err_t zv_ui_show_link_state(zv_link_state_t state, uint32_t retry_ms, uint8_t wifi_signal_bars)
{
    static char date_buf[16];
    static char clock_buf[12];
    zv_ui_canvas_t canvas = {.ctx = NULL, .fill_rect = canvas_fill, .draw_row = canvas_row};
    zv_ui_chrome_t chrome;
    zv_face_frame_t face;
    char notice[48];
    const zv_avatar_state_t avatar = (state == ZV_LINK_ONLINE || state == ZV_LINK_RECONNECTING) ? ZV_AVATAR_IDLE :
                                     state == ZV_LINK_CONNECTING ? ZV_AVATAR_THINKING : ZV_AVATAR_ERROR;

    ESP_RETURN_ON_ERROR(ensure_status_ui_ready(), TAG, "link UI init failed");
    s_last_link_bars = wifi_signal_bars > 3u ? 3u : wifi_signal_bars;
    if (zv_ui_settings_active()) {
        return zv_ui_settings_resume(s_last_link_bars, state == ZV_LINK_ONLINE);
    }

    memset(&chrome, 0, sizeof(chrome));
    chrome.brand_text = NULL;
    chrome.use_zenti_head = true;
    chrome.show_home = true;
    chrome.status_text = zv_link_state_name(state);
    fill_status_chrome(&chrome, date_buf, sizeof(date_buf), clock_buf, sizeof(clock_buf));
    chrome.signal_bars = wifi_signal_bars > 3u ? 3u : wifi_signal_bars;
    s_last_link_bars = chrome.signal_bars;
    chrome.secure_link = state == ZV_LINK_ONLINE;
    chrome.gateway_online = state == ZV_LINK_ONLINE;
    if (state == ZV_LINK_OFFLINE || state == ZV_LINK_DEGRADED) {
        (void)snprintf(notice, sizeof(notice), "LOCAL ONLY - RETRY %lu MS", (unsigned long)retry_ms);
        chrome.notice = notice;
        chrome.detail_text = chrome.signal_bars == 0u ? "WIFI UNAVAILABLE" : "GATEWAY UNAVAILABLE";
    } else if (state == ZV_LINK_CONNECTING || state == ZV_LINK_RECONNECTING) {
        chrome.notice = "GETTING READY";
        chrome.detail_text = chrome.signal_bars == 0u ? "JOINING WIFI" : "SECURING GATEWAY";
    } else {
        chrome.detail_text = "SECURE SESSION";
    }
    ESP_RETURN_ON_ERROR(zv_to_esp(zv_avatar_set_state(&s_status_model, avatar, 0u)), TAG, "link UI state failed");
    ESP_RETURN_ON_ERROR(zv_to_esp(zv_avatar_frame(&s_status_model, 0u, NULL, 0u, &face)), TAG, "link UI frame failed");
    return zv_to_esp(zv_ui_paint_frame(&canvas, &face, avatar, &chrome));
}

esp_err_t zv_ui_show_voice_state(zv_avatar_state_t state, const char *status)
{
    ESP_RETURN_ON_ERROR(ensure_status_ui_ready(), TAG, "voice UI init failed");
    s_voice_state = state;
    uint32_t stamp = (uint32_t)(esp_timer_get_time() / 1000);
    s_voice_refresh_ms = s_voice_tick_ms = stamp;
    s_voice_thinking_phase = UINT8_MAX;
    zv_envelope_config_t envelope_config = voice_envelope_config(state);
    (void)zv_envelope_init(&s_voice_envelope, &envelope_config);
    static char date_buf[16];
    static char clock_buf[12];
    zv_ui_canvas_t canvas = {.ctx = NULL, .fill_rect = canvas_fill, .draw_row = canvas_row};
    zv_ui_chrome_t chrome;
    zv_face_frame_t face;
    memset(&chrome, 0, sizeof(chrome));
    chrome.use_zenti_head = true;
    chrome.show_home = true;
    chrome.status_text = status;
    chrome.show_cancel = state == ZV_AVATAR_LISTENING || state == ZV_AVATAR_THINKING ||
                         state == ZV_AVATAR_SPEAKING || state == ZV_AVATAR_EXECUTING;
    fill_status_chrome(&chrome, date_buf, sizeof(date_buf), clock_buf, sizeof(clock_buf));
    chrome.signal_bars = s_last_link_bars;
    chrome.secure_link = true;
    chrome.gateway_online = true;
    ESP_RETURN_ON_ERROR(zv_to_esp(zv_avatar_set_state(&s_status_model, state, stamp)), TAG,
                        "voice UI state failed");
    ESP_RETURN_ON_ERROR(zv_to_esp(zv_avatar_frame(&s_status_model, stamp, NULL, 0u, &face)), TAG,
                        "voice UI frame failed");
    /* Eyes are a static snapshot: later voice patches exclude this region.
     * Preserve the state's expression rather than freezing a transient blink. */
    face.eye_open = zv_avatar_expression(state)->eye_open;
    ESP_RETURN_ON_ERROR(zv_to_esp(zv_ui_paint_frame(&canvas, &face, state, &chrome)), TAG,
                        "voice UI paint failed");
    remember_voice_header(&chrome);
    return ESP_OK;
}

#if CONFIG_ZV_M3A_VOICE_MODE
typedef struct { int16_t x0, y0, x1, y1; } voice_clip_t;
static zv_status_t voice_clip_fill(void *ctx, int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color)
{
    const voice_clip_t *clip = ctx;
    if (x0 < clip->x0) x0 = clip->x0;
    if (y0 < clip->y0) y0 = clip->y0;
    if (x1 > clip->x1) x1 = clip->x1;
    if (y1 > clip->y1) y1 = clip->y1;
    return x0 < x1 && y0 < y1 ? canvas_fill(NULL, x0, y0, x1, y1, color) : ZV_OK;
}
static zv_status_t voice_clip_row(void *ctx, int16_t x, int16_t y, uint16_t width, const uint16_t *pixels)
{
    const voice_clip_t *clip = ctx;
    if (y < clip->y0 || y >= clip->y1) return ZV_OK;
    int16_t left = x > clip->x0 ? x : clip->x0;
    int16_t right = x + width < clip->x1 ? x + width : clip->x1;
    return left < right ? canvas_row(NULL, left, y, (uint16_t)(right-left), pixels+(left-x)) : ZV_OK;
}

/* Compose in RAM. Forwarding each primitive to the LCD exposes the background
 * clear as a flashing box and blocks the audio producer on hundreds of writes.
 * The larger waveform patch is 240*14 pixels; no full framebuffer is needed. */
static uint16_t s_voice_patch[240u * 14u];
static zv_status_t voice_patch_fill(void *ctx, int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color)
{
    const voice_clip_t *clip = ctx;
    if (x0 < clip->x0) x0 = clip->x0;
    if (y0 < clip->y0) y0 = clip->y0;
    if (x1 > clip->x1) x1 = clip->x1;
    if (y1 > clip->y1) y1 = clip->y1;
    const size_t stride = (size_t)(clip->x1 - clip->x0);
    for (int16_t y = y0; y < y1; ++y)
        for (int16_t x = x0; x < x1; ++x)
            s_voice_patch[(size_t)(y - clip->y0) * stride + (size_t)(x - clip->x0)] = color;
    return ZV_OK;
}
static zv_status_t voice_patch_row(void *ctx, int16_t x, int16_t y, uint16_t width, const uint16_t *pixels)
{
    const voice_clip_t *clip = ctx;
    if (y < clip->y0 || y >= clip->y1) return ZV_OK;
    int16_t left = x > clip->x0 ? x : clip->x0;
    int16_t right = x + width < clip->x1 ? x + width : clip->x1;
    if (left < right)
        memcpy(s_voice_patch + (size_t)(y - clip->y0) * (size_t)(clip->x1 - clip->x0) +
               (size_t)(left - clip->x0), pixels + (left - x), (size_t)(right - left) * sizeof(uint16_t));
    return ZV_OK;
}

static esp_err_t voice_paint_patch(const voice_clip_t *region, const zv_face_frame_t *face,
                                  zv_avatar_state_t state, const zv_ui_chrome_t *chrome)
{
    zv_ui_canvas_t canvas = {.ctx=(void *)region, .fill_rect=voice_patch_fill, .draw_row=voice_patch_row};
    ESP_RETURN_ON_ERROR(zv_to_esp(zv_ui_paint_frame(&canvas, face, state, chrome)), TAG, "voice partial paint");
    return zv_board_lcd_draw_patch((uint16_t)region->x0, (uint16_t)region->y0,
        (uint16_t)(region->x1-region->x0), (uint16_t)(region->y1-region->y0), s_voice_patch);
}

#endif

esp_err_t zv_ui_voice_tick(void)
{
#if CONFIG_ZV_M3A_VOICE_MODE
    if (s_voice_state != ZV_AVATAR_THINKING || zv_ui_settings_active()) return ESP_OK;
    uint32_t stamp = (uint32_t)(esp_timer_get_time() / 1000);
    if ((uint32_t)(stamp-s_voice_tick_ms) < 100u) return ESP_OK;
    s_voice_tick_ms = stamp;
    zv_face_frame_t face;
    ESP_RETURN_ON_ERROR(zv_to_esp(zv_avatar_frame(&s_status_model, stamp, NULL, 0u, &face)), TAG, "thinking frame");
    uint8_t phase = (uint8_t)(face.ring_sweep_deg / 120u);
    if (phase == s_voice_thinking_phase) return ESP_OK;
    const voice_clip_t region = {60,266,300,280};
    zv_ui_chrome_t chrome = {.use_zenti_head=true};
    ESP_RETURN_ON_ERROR(voice_paint_patch(&region, &face, ZV_AVATAR_THINKING, &chrome), TAG, "thinking patch");
    s_voice_thinking_phase = phase;
#endif
    return ESP_OK;
}

esp_err_t zv_ui_voice_audio(zv_avatar_state_t state, const int16_t *pcm, size_t count)
{
#if CONFIG_ZV_M3A_VOICE_MODE
    uint16_t level = 0;
    ESP_RETURN_ON_ERROR(zv_to_esp(zv_envelope_process_pcm16(&s_voice_envelope, pcm, count, &level)), TAG, "voice envelope");
    uint32_t stamp = (uint32_t)(esp_timer_get_time() / 1000);
    if ((uint32_t)(stamp-s_voice_audio_ms) < 100u || zv_ui_settings_active()) return ESP_OK;
    s_voice_audio_ms = stamp;
    zv_mouth_shape_t mouth;
    ESP_RETURN_ON_ERROR(zv_to_esp(zv_lipsync_mouth(NULL, 0u, level, false, &mouth)), TAG, "voice mouth");
    zv_face_frame_t face;
    ESP_RETURN_ON_ERROR(zv_to_esp(zv_avatar_frame(&s_status_model, stamp, &mouth, level, &face)), TAG, "voice frame");
    zv_ui_chrome_t chrome = {.use_zenti_head=true, .show_wave=true, .wave_len=ZV_UI_WAVE_BARS};
    for (size_t i=0; i<ZV_UI_WAVE_BARS; ++i) chrome.wave[i]=(int16_t)level;
    /* Only two completed patches reach the LCD, never an intermediate erase. */
    voice_clip_t regions[] = {{164,230,197,253}, {60,266,300,280}};
    if (zv_ui_nous_selected()) regions[0]=(voice_clip_t){164,208,197,234};
    /* Listening only updates the energy meter; its mouth is not lip-synced. */
    for (size_t i=state == ZV_AVATAR_SPEAKING ? 0u : 1u; i<2; ++i) {
        ESP_RETURN_ON_ERROR(voice_paint_patch(&regions[i], &face, state, &chrome), TAG, "voice patch transfer");
    }
#else
    (void)state; (void)pcm; (void)count;
#endif
    return ESP_OK;
}

esp_err_t zv_ui_voice_refresh(uint8_t signal_bars)
{
#if CONFIG_ZV_M3A_VOICE_MODE
    uint32_t stamp = (uint32_t)(esp_timer_get_time() / 1000);
    if ((uint32_t)(stamp-s_voice_refresh_ms) < 1000u || zv_ui_settings_active()) return ESP_OK;
    s_voice_refresh_ms = stamp;
    s_last_link_bars = signal_bars > 3u ? 3u : signal_bars;
    char date[16], clock[16];
    zv_ui_chrome_t chrome = {.use_zenti_head=true, .show_connectivity=true,
        .signal_bars=s_last_link_bars, .secure_link=true, .gateway_online=true};
    fill_status_chrome(&chrome, date, sizeof(date), clock, sizeof(clock));
    bool clock_changed = !s_voice_header.valid ||
        strcmp(s_voice_header.date, chrome.date_text) != 0 ||
        strcmp(s_voice_header.clock, chrome.clock_text) != 0;
    bool battery_changed = !s_voice_header.valid ||
        s_voice_header.battery_ok != chrome.battery_ok ||
        s_voice_header.battery_percent != chrome.battery_percent;
    /* Require three consistent readings before changing a known percentage.
     * Unknown/available transitions and critical readings remain immediate. */
    if (battery_changed && s_voice_header.valid && chrome.battery_ok &&
        s_voice_header.battery_ok && chrome.battery_percent > ZV_UI_BATTERY_CRITICAL_PCT) {
        if (s_voice_header.pending_samples == 0u ||
            s_voice_header.pending_battery != chrome.battery_percent) {
            s_voice_header.pending_battery = chrome.battery_percent;
            s_voice_header.pending_samples = 1u;
        } else if (s_voice_header.pending_samples < 3u) {
            ++s_voice_header.pending_samples;
        }
        if (s_voice_header.pending_samples < 3u) {
            chrome.battery_percent = s_voice_header.battery_percent;
            battery_changed = false;
        }
    } else {
        s_voice_header.pending_samples = 0u;
    }
    bool signal_changed = !s_voice_header.valid ||
        s_voice_header.signal_bars != chrome.signal_bars;
    if (!clock_changed && !battery_changed && !signal_changed) return ESP_OK;
    zv_face_frame_t face;
    ESP_RETURN_ON_ERROR(zv_to_esp(zv_avatar_frame(&s_status_model, stamp, NULL, 0u, &face)), TAG, "voice clock frame");
    /* Never erase the lock for battery or Wi-Fi updates. Include the percentage
     * below the battery and its small critical warning inside the lock. */
    voice_clip_t regions[] = {{96,0,258,80}, {52,56,96,84},
                             {278,58,314,92}, {269,65,272,77}};
    for (size_t i = 0; i < 4u; ++i) {
        if ((i == 0u && !clock_changed) || (i == 1u && !signal_changed) ||
            (i >= 2u && !battery_changed)) continue;
        zv_ui_canvas_t canvas = {.ctx=&regions[i],.fill_rect=voice_clip_fill,.draw_row=voice_clip_row};
        ESP_RETURN_ON_ERROR(zv_to_esp(zv_ui_paint_frame(&canvas,&face,s_voice_state,&chrome)), TAG,
                            "voice header paint failed");
    }
    remember_voice_header(&chrome);
    return ESP_OK;
#else
    (void)signal_bars;
    return ESP_OK;
#endif
}

esp_err_t zv_ui_show_booting(void)
{
    static char date_buf[16];
    static char clock_buf[12];
    zv_ui_canvas_t canvas = {.ctx = NULL, .fill_rect = canvas_fill, .draw_row = canvas_row};
    zv_ui_chrome_t chrome;
    zv_face_frame_t face;

    ESP_RETURN_ON_ERROR(ensure_status_ui_ready(), TAG, "boot UI init failed");
    memset(&chrome, 0, sizeof(chrome));
    chrome.brand_text = NULL;
    chrome.use_zenti_head = true;
    chrome.show_home = true;
    chrome.show_connectivity = true;
    chrome.status_text = "STARTING";
    fill_status_chrome(&chrome, date_buf, sizeof(date_buf), clock_buf, sizeof(clock_buf));
    chrome.signal_bars = 0u;
    chrome.secure_link = false;
    chrome.gateway_online = false;
    ESP_RETURN_ON_ERROR(zv_to_esp(zv_avatar_set_state(&s_status_model, ZV_AVATAR_IDLE, 0u)), TAG,
                        "boot UI state failed");
    ESP_RETURN_ON_ERROR(zv_to_esp(zv_avatar_frame(&s_status_model, 0u, NULL, 0u, &face)), TAG,
                        "boot UI frame failed");
    return zv_to_esp(zv_ui_paint_frame(&canvas, &face, ZV_AVATAR_IDLE, &chrome));
}

static void synth_wave(zv_ui_chrome_t *chrome, uint32_t now_ms, uint16_t level)
{
    chrome->show_wave = true;
    chrome->wave_len = ZV_UI_WAVE_BARS;
    for (int i = 0; i < ZV_UI_WAVE_BARS; i++) {
        /* Integer sine via cos table phase; no floating point. */
        uint32_t phase = (now_ms / 4u) + (uint32_t)i * 17u;
        int32_t swing = (int32_t)(level) * ((int32_t)(phase % 180u) - 90) / 90;
        chrome->wave[i] = (int16_t)swing;
    }
}

static esp_err_t wait_tap(uint16_t *out_x, uint16_t *out_y)
{
    uint16_t x = 0, y = 0;
    ESP_RETURN_ON_ERROR(zv_board_touch_wait_release(5000), TAG, "previous touch never released");
    for (uint32_t waited = 0; waited < UI_CONFIRM_TIMEOUT_MS; waited += UI_TOUCH_POLL_MS) {
        bool touched = false;
        ESP_RETURN_ON_ERROR(zv_board_touch_poll(&touched, &x, &y), TAG, "touch poll failed");
        if (touched) {
            if (out_x != NULL) *out_x = x;
            if (out_y != NULL) *out_y = y;
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(UI_TOUCH_POLL_MS));
    }
    return ESP_ERR_TIMEOUT;
}

static bool tap_is_home(uint16_t x, uint16_t y)
{
    return zv_ui_touch_is_home(x, y);
}

static bool tap_is_approve(uint16_t x, uint16_t y)
{
    return x >= ZV_UI_APPROVE_X0 && x <= ZV_UI_APPROVE_X1 &&
           y >= ZV_UI_APPROVE_Y0 && y <= ZV_UI_APPROVE_Y1;
}

static bool tap_is_deny(uint16_t x, uint16_t y)
{
    return x >= ZV_UI_DENY_X0 && x <= ZV_UI_DENY_X1 &&
           y >= ZV_UI_DENY_Y0 && y <= ZV_UI_DENY_Y1;
}

static esp_err_t present_color_calibration(void)
{
    zv_ui_canvas_t canvas = {.ctx = NULL, .fill_rect = canvas_fill, .draw_row = canvas_row};
    static const struct {
        int16_t x0, y0, x1, y1;
        uint16_t rgb565;
        const char *label;
        uint16_t text_rgb565;
    } swatches[] = {
        /* Raw RGB565 primaries make channel/order faults unambiguous. */
        {32, 72, 174, 132, 0x001Fu, "BLUE", 0xFFFFu},
        {186, 72, 328, 132, 0x07FFu, "CYAN", 0x0000u},
        {32, 144, 174, 204, 0xFFFFu, "WHITE", 0x0000u},
        {186, 144, 328, 204, 0x07E0u, "GREEN", 0x0000u},
        {32, 216, 174, 276, 0xF800u, "RED", 0xFFFFu},
        {186, 216, 328, 276, 0xF81Fu, "MAGENTA", 0xFFFFu},
    };

    ESP_RETURN_ON_ERROR(zv_board_lcd_fill_rect(0, 0, ZV_UI_W, ZV_UI_H,
                                               zv_rgb888_to_rgb565(ZV_COLOR_BACKGROUND)),
                        TAG, "calibration clear failed");
    ESP_RETURN_ON_ERROR(zv_to_esp(zv_ui_paint_text(&canvas, 78, 34, "COLOR CHECK", 0xFFFFu, 2u)),
                        TAG, "calibration title failed");

    for (size_t i = 0; i < sizeof(swatches) / sizeof(swatches[0]); i++) {
        const uint16_t color = swatches[i].rgb565;
        ESP_RETURN_ON_ERROR(zv_board_lcd_fill_rect((uint16_t)swatches[i].x0, (uint16_t)swatches[i].y0,
                                                   (uint16_t)swatches[i].x1, (uint16_t)swatches[i].y1, color),
                            TAG, "calibration swatch failed");
        const int16_t label_w = zv_ui_text_width(swatches[i].label, 2u);
        const int16_t label_x = (int16_t)((swatches[i].x0 + swatches[i].x1 - label_w) / 2);
        const int16_t label_y = (int16_t)(swatches[i].y0 + 22);
        ESP_RETURN_ON_ERROR(zv_to_esp(zv_ui_paint_text(&canvas, label_x, label_y, swatches[i].label,
                                                        swatches[i].text_rgb565, 2u)),
                            TAG, "calibration label failed");
    }

    ESP_RETURN_ON_ERROR(zv_to_esp(zv_ui_paint_text(&canvas, 72, 304, "TAP IF COLORS MATCH", 0xFFFFu, 1u)),
                        TAG, "calibration prompt failed");
    ESP_LOGI(TAG, "UI color calibration displayed: BLUE CYAN WHITE GREEN RED MAGENTA; tap only if labels match");
    ESP_RETURN_ON_ERROR(wait_tap(NULL, NULL), TAG, "color calibration confirm failed");
    ESP_LOGI(TAG, "UI color calibration confirmed");
    return ESP_OK;
}

static esp_err_t present_menu_overlay(zv_avatar_model_t *model, uint32_t now_ms)
{
    static char date_buf[16];
    static char clock_buf[12];
    zv_ui_canvas_t canvas = {.ctx = NULL, .fill_rect = canvas_fill, .draw_row = canvas_row};
    zv_ui_chrome_t chrome;
    memset(&chrome, 0, sizeof(chrome));
    chrome.brand_text = NULL;
    chrome.menu_open = true;
    chrome.use_zenti_head = true;
    chrome.show_home = true;
    fill_status_chrome(&chrome, date_buf, sizeof(date_buf), clock_buf, sizeof(clock_buf));
    chrome.menu_count = 5;
    chrome.menu_items[0] = "MUTE";
    chrome.menu_items[1] = "BRIGHTNESS";
    chrome.menu_items[2] = "WIFI + LINK";
    chrome.menu_items[3] = "DEVICE INFO";
    chrome.menu_items[4] = "RESTART";
    chrome.status_text = "MENU";
    ESP_RETURN_ON_ERROR(zv_to_esp(zv_avatar_set_state(model, ZV_AVATAR_IDLE, now_ms)), TAG, "menu state failed");
    zv_face_frame_t face;
    ESP_RETURN_ON_ERROR(zv_to_esp(zv_avatar_frame(model, now_ms, NULL, 0, &face)), TAG, "menu frame failed");
    ESP_RETURN_ON_ERROR(zv_to_esp(zv_ui_paint_frame(&canvas, &face, ZV_AVATAR_IDLE, &chrome)), TAG, "menu paint failed");
    ESP_LOGI(TAG, "UI menu overlay painted (home); tap to close");
    uint16_t tap_x = 0, tap_y = 0;
    esp_err_t tap_err = wait_tap(&tap_x, &tap_y);
    if (tap_err != ESP_OK) return tap_err;

    /* A tap must visibly dismiss the overlay; merely logging it left MENU on-screen. */
    memset(&chrome, 0, sizeof(chrome));
    chrome.brand_text = NULL;
    chrome.use_zenti_head = true;
    chrome.show_home = true;
    chrome.show_connectivity = true;
    chrome.signal_bars = 3u;
    chrome.secure_link = true;
    chrome.gateway_online = true;
    chrome.status_text = "IDLE";
    chrome.detail_text = "READY";
    fill_status_chrome(&chrome, date_buf, sizeof(date_buf), clock_buf, sizeof(clock_buf));
    ESP_RETURN_ON_ERROR(zv_to_esp(zv_avatar_set_state(model, ZV_AVATAR_IDLE, now_ms)), TAG, "menu close state failed");
    ESP_RETURN_ON_ERROR(zv_to_esp(zv_avatar_frame(model, now_ms, NULL, 0, &face)), TAG, "menu close frame failed");
    ESP_RETURN_ON_ERROR(zv_to_esp(zv_ui_paint_frame(&canvas, &face, ZV_AVATAR_IDLE, &chrome)), TAG,
                        "menu close paint failed");
    ESP_LOGI(TAG, "UI menu closed by panel touch at x=%u y=%u", tap_x, tap_y);
    return ESP_OK;
}

esp_err_t zv_ui_service_status_menu(void)
{
    ESP_RETURN_ON_ERROR(ensure_status_ui_ready(), TAG, "menu UI init failed");
    return present_menu_overlay(&s_status_model, 0u);
}

static esp_err_t present_state(zv_avatar_model_t *model, zv_avatar_state_t state, uint32_t *now_ms,
                               const char *notice)
{
    static char date_buf[16];
    static char clock_buf[12];
    zv_ui_canvas_t canvas = {.ctx = NULL, .fill_rect = canvas_fill, .draw_row = canvas_row};
    zv_ui_chrome_t chrome;
    memset(&chrome, 0, sizeof(chrome));
    chrome.brand_text = NULL;
    chrome.use_zenti_head = true;
    chrome.show_home = true;
    chrome.status_text = zv_avatar_state_name(state);
    chrome.mic_muted = false;
    fill_status_chrome(&chrome, date_buf, sizeof(date_buf), clock_buf, sizeof(clock_buf));
    chrome.notice = notice;
    chrome.detail_text = state == ZV_AVATAR_IDLE ? "READY" : "VOICE ASSISTANT";
    chrome.signal_bars = 3u;
    chrome.secure_link = true;
    chrome.gateway_online = true;

    ESP_RETURN_ON_ERROR(zv_to_esp(zv_avatar_set_state(model, state, *now_ms)), TAG, "avatar set state failed");

    for (uint32_t held = 0; held < UI_STATE_HOLD_MS; held += UI_FRAME_MS) {
        uint16_t level = 0;
        if (state == ZV_AVATAR_SPEAKING || state == ZV_AVATAR_LISTENING) {
            /* Synthetic 2 Hz amplitude so lip-sync and the wave layer move. */
            uint32_t t = *now_ms / 500u;
            level = (uint16_t)(300u + (t % 2u) * 500u);
            synth_wave(&chrome, *now_ms, level);
        } else {
            chrome.show_wave = false;
        }
        zv_face_frame_t face;
        zv_mouth_shape_t mouth = {.open = level, .width = 520u, .round = 100u};
        ESP_RETURN_ON_ERROR(zv_to_esp(zv_avatar_frame(model, *now_ms, (state == ZV_AVATAR_SPEAKING) ? &mouth : NULL,
                                                      level, &face)),
                            TAG, "avatar frame failed");
        ESP_RETURN_ON_ERROR(zv_to_esp(zv_ui_paint_frame(&canvas, &face, state, &chrome)), TAG, "paint failed");
        *now_ms += UI_FRAME_MS;
        vTaskDelay(pdMS_TO_TICKS(UI_FRAME_MS));
    }

    if (state == ZV_AVATAR_APPROVAL) {
        ESP_LOGI(TAG, "UI state %s painted; tap APPROVE or DENY to confirm the dedicated controls",
                 zv_avatar_state_name(state));
    } else {
        ESP_LOGI(TAG, "UI state %s painted; tap the panel to confirm, or tap menu",
                 zv_avatar_state_name(state));
    }
confirm_touch: ;
    uint16_t tx = 0, ty = 0;
    esp_err_t tap_err = wait_tap(&tx, &ty);
    if (tap_err != ESP_OK) {
        return tap_err;
    }
    if (tap_is_home(tx, ty)) {
        ESP_RETURN_ON_ERROR(present_menu_overlay(model, *now_ms), TAG, "menu interaction failed");
        zv_face_frame_t restored;
        ESP_RETURN_ON_ERROR(zv_to_esp(zv_avatar_set_state(model, state, *now_ms)), TAG, "restore state failed");
        ESP_RETURN_ON_ERROR(zv_to_esp(zv_avatar_frame(model, *now_ms, NULL, 0u, &restored)), TAG, "restore frame failed");
        ESP_RETURN_ON_ERROR(zv_to_esp(zv_ui_paint_frame(&canvas, &restored, state, &chrome)), TAG, "restore paint failed");
        goto confirm_touch;
    }
    if (state == ZV_AVATAR_APPROVAL) {
        if (tap_is_approve(tx, ty)) {
            ESP_LOGI(TAG, "UI approval APPROVE control confirmed (visual acceptance only; no action executed)");
            return ESP_OK;
        }
        if (tap_is_deny(tx, ty)) {
            ESP_LOGI(TAG, "UI approval DENY control confirmed (visual acceptance only; no action executed)");
            return ESP_OK;
        }
        ESP_LOGW(TAG, "approval visual acceptance requires tapping APPROVE or DENY");
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

esp_err_t zv_ui_acceptance_demo(void)
{
    ESP_LOGW(TAG, "UI acceptance mode enabled: eight avatar states + menu on the reference painter; "
                  "network, gateway, Hermes, credentials, OTA/eFuse, erase remain gated");

    /*
     * M0 runs first (selected by Kconfig). It has already initialized the
     * panel and required a positional challenge, so a blank panel cannot
     * reach this demo.
     */
    ESP_RETURN_ON_ERROR(zv_board_m0_acceptance_demo(), TAG, "M0 display challenge failed under UI mode");
    ESP_RETURN_ON_ERROR(zv_board_lcd_init(), TAG, "LCD re-init failed");
    ESP_RETURN_ON_ERROR(zv_board_touch_init(), TAG, "touch re-init failed");
    ESP_RETURN_ON_ERROR(zv_board_backlight_set(80), TAG, "backlight failed");
    ESP_RETURN_ON_ERROR(present_color_calibration(), TAG, "UI color calibration failed");

    {
        char date_buf[16];
        char clock_buf[12];
        if (format_datetime(date_buf, sizeof(date_buf), clock_buf, sizeof(clock_buf))) {
            ESP_LOGI(TAG, "UI date/time available: %s | %s", date_buf, clock_buf);
        } else {
            ESP_LOGW(TAG, "UI date/time not set: %s | %s", date_buf, "TIME NOT SET");
        }
    }

    zv_avatar_model_t model;
    uint32_t now_ms = 0;
    ESP_RETURN_ON_ERROR(zv_to_esp(zv_avatar_init(&model, now_ms, 0xA5A5A5A5u)), TAG, "avatar init failed");

    static const zv_avatar_state_t k_states[ZV_AVATAR_STATE_COUNT] = {
        ZV_AVATAR_IDLE,    ZV_AVATAR_LISTENING, ZV_AVATAR_THINKING, ZV_AVATAR_SPEAKING,
        ZV_AVATAR_EXECUTING, ZV_AVATAR_SUCCESS, ZV_AVATAR_ERROR,    ZV_AVATAR_APPROVAL,
    };

    for (unsigned i = 0; i < (unsigned)ZV_AVATAR_STATE_COUNT; i++) {
        const char *notice = NULL;
        if (k_states[i] == ZV_AVATAR_APPROVAL) {
            notice = "DELETE ALL KEYS";
        }
        esp_err_t err = present_state(&model, k_states[i], &now_ms, notice);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "UI acceptance failed on state %s: %s", zv_avatar_state_name(k_states[i]),
                     esp_err_to_name(err));
            return err;
        }
        ESP_LOGI(TAG, "UI state %u of %d confirmed: %s", i + 1u, (int)ZV_AVATAR_STATE_COUNT,
                 zv_avatar_state_name(k_states[i]));
    }

    /* End on the menu, then verify that a touch visibly returns to the home face. */
    ESP_RETURN_ON_ERROR(present_menu_overlay(&model, now_ms), TAG, "menu close interaction failed");

    ESP_RETURN_ON_ERROR(zv_board_backlight_set(35), TAG, "backlight dim failed");
    ESP_LOGI(TAG, "UI acceptance passed: %d avatar states + menu confirmed; platform remains gated",
             (int)ZV_AVATAR_STATE_COUNT);
    return ESP_OK;
}

#else /* no UI or WSS acceptance */

esp_err_t zv_ui_voice_tick(void) { return ESP_OK; }

esp_err_t zv_ui_show_voice_state(zv_avatar_state_t state, const char *status)
{
    (void)state;
    (void)status;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t zv_ui_acceptance_demo(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t zv_ui_show_link_state(zv_link_state_t state, uint32_t retry_ms, uint8_t wifi_signal_bars)
{
    (void)state;
    (void)retry_ms;
    (void)wifi_signal_bars;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t zv_ui_show_booting(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t zv_ui_service_status_menu(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
