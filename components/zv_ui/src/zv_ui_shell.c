/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 *
 * Interactive menu shell. Default OFF; selected by ZV_UI_SHELL_MODE.
 *
 * The shell owns three things the static demo never had: a real focus ring the
 * operator drives, a volume/mute state that survives between screens, and a
 * clock that is refreshed on a bounded cadence instead of only when something
 * else happens to trigger a repaint.
 *
 * It is a lab aid. Network, gateway, Hermes, credentials, OTA and eFuse remain
 * gated, and the only destructive action reachable from here is a restart the
 * operator has to confirm on a second screen.
 */
#include "zv_ui.h"

#if CONFIG_ZV_UI_SHELL_MODE || CONFIG_ZV_M3A_VOICE_MODE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "zv_avatar_model.h"
#include "zv_board.h"
#include "zv_status.h"
#include "zv_touch.h"
#include "zv_ui_draw.h"
#include "zv_volume.h"

static const char *TAG = "zv_shell";

#define UI_SHELL_POLL_MS 40u
/** How long a transient notice stays on screen. */
#define UI_SHELL_NOTICE_MS 2500u

typedef enum {
    SHELL_ITEM_NETWORK = 0,
    SHELL_ITEM_VOLUME,
    SHELL_ITEM_MUTE,
    SHELL_ITEM_RESTART,
    SHELL_ITEM_OPTIONS,
    SHELL_ITEM_COUNT,
} shell_item_t;

/*
 * The painter, the touch recognizer and this list must agree on the row count,
 * so it is asserted rather than discovered at runtime.
 */
_Static_assert((int)SHELL_ITEM_COUNT <= (int)ZV_UI_MAX_MENU, "shell menu must fit the painter");
_Static_assert((int)SHELL_ITEM_COUNT <= (int)ZV_TOUCH_MENU_MAX, "shell menu must fit the touch recognizer");

static const char *const k_items[SHELL_ITEM_COUNT] = {"NETWORK", "VOLUME", "MUTE", "RESTART", "AVATAR / CHAT"};

static bool s_reset_conversation;
bool zv_ui_take_conversation_reset(void) { bool requested=s_reset_conversation; s_reset_conversation=false; return requested; }

static void settings_save_avatar(void)
{
    nvs_handle_t handle;
    if (nvs_open("zv_ui", NVS_READWRITE, &handle) == ESP_OK) {
        if (nvs_set_str(handle,"avatar",zv_ui_nous_selected()?"nous-girl":"zenti") != ESP_OK || nvs_commit(handle) != ESP_OK) ESP_LOGW(TAG,"avatar persistence unavailable");
        nvs_close(handle);
    } else ESP_LOGW(TAG,"avatar persistence unavailable");
}

static struct {
    bool ready;
    bool voice_mode, online, ignore_contact, force_paint;
    uint8_t link_bars;
    zv_avatar_model_t model;
    zv_volume_t volume;
    zv_volume_t saved_volume;
    zv_repaint_t repaint;
    zv_touch_t touch;
    zv_ui_screen_t screen;
    uint8_t focus;
    char clock[12];
    char notice[48];
    uint32_t notice_until_ms;
} s;

/* One two-byte record keeps level/mute consistent. Missing or invalid data
 * falls back to the configured level; never erase NVS to repair UI settings. */
static void settings_load_volume(void)
{
    uint8_t record[2];
    size_t length = sizeof(record);
    nvs_handle_t handle;
    zv_volume_init(&s.volume, CONFIG_ZV_UI_DEFAULT_VOLUME);
    if (nvs_open("zv_ui", NVS_READONLY, &handle) == ESP_OK) {
        if (nvs_get_blob(handle, "volume", record, &length) == ESP_OK &&
            length == sizeof(record) && record[0] <= ZV_VOLUME_MAX && record[1] <= 1u) {
            (void)zv_volume_set_level(&s.volume, record[0]);
            (void)zv_volume_set_muted(&s.volume, record[1] != 0u);
        }
        nvs_close(handle);
    }
    s.saved_volume = s.volume;
}

/* Called on release, never for drag previews or periodic display refreshes. */
static void settings_save_volume(void)
{
    if (s.volume.level == s.saved_volume.level && s.volume.muted == s.saved_volume.muted) return;
    nvs_handle_t handle;
    if (nvs_open("zv_ui", NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGW(TAG, "volume persistence unavailable");
        return;
    }
    const uint8_t record[2] = {s.volume.level, s.volume.muted ? 1u : 0u};
    if (nvs_set_blob(handle, "volume", record, sizeof(record)) == ESP_OK && nvs_commit(handle) == ESP_OK) {
        s.saved_volume = s.volume;
    } else ESP_LOGW(TAG, "volume persistence unavailable");
    nvs_close(handle);
}

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/** Board I/O uses esp_err_t; the portable painter canvas uses zv_status_t. */
typedef struct { int16_t x0, y0, x1, y1; } settings_clip_t;

static zv_status_t canvas_fill(void *ctx, int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color)
{
    const settings_clip_t *clip = ctx;
    if (clip != NULL) {
        if (x0 < clip->x0) x0 = clip->x0;
        if (y0 < clip->y0) y0 = clip->y0;
        if (x1 > clip->x1) x1 = clip->x1;
        if (y1 > clip->y1) y1 = clip->y1;
        if (x0 >= x1 || y0 >= y1) return ZV_OK;
    }
    if (zv_board_lcd_fill_rect((uint16_t)x0, (uint16_t)y0, (uint16_t)x1, (uint16_t)y1, color) == ESP_OK) {
        return ZV_OK;
    }
    return ZV_ERR_HARDWARE_GATE;
}

static zv_status_t canvas_row(void *ctx, int16_t x, int16_t y, uint16_t width, const uint16_t *pixels)
{
    const settings_clip_t *clip = ctx;
    if (clip != NULL) {
        if (y < clip->y0 || y >= clip->y1) return ZV_OK;
        int16_t left = x > clip->x0 ? x : clip->x0;
        int16_t right = x + width < clip->x1 ? (int16_t)(x + width) : clip->x1;
        if (left >= right) return ZV_OK;
        pixels += left - x;
        x = left;
        width = (uint16_t)(right - left);
    }
    return zv_board_lcd_draw_row((uint16_t)x, (uint16_t)y, width, pixels) == ESP_OK
        ? ZV_OK : ZV_ERR_HARDWARE_GATE;
}

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

/**
 * Applies CONFIG_ZV_UI_TZ before the first clock render.
 *
 * SNTP hands us UTC seconds; localtime_r() is what turns them into the time the
 * operator expects, and without TZ it would silently stay on UTC and be wrong
 * by the offset for half the year. The zone is a build variable so a unit can be
 * shipped to its own region without a code change.
 */
static void apply_timezone(void)
{
    setenv("TZ", CONFIG_ZV_UI_TZ, 1);
    tzset();
    ESP_LOGI(TAG, "timezone applied: TZ=%s", CONFIG_ZV_UI_TZ);
}

/** True wall-clock only after SNTP (or other) sets time past 2020-01-01. */
static void format_clock(char *buf, size_t buf_len)
{
    time_t now = time(NULL);
    struct tm tmv;
    if (buf == NULL) return;
    if (buf_len < 9u || now < 1577836800 || localtime_r(&now, &tmv) == NULL) {
        if (buf_len > 0u) {
            buf[0] = '\0';
        }
        return;
    }
    const int hour12 = tmv.tm_hour % 12 == 0 ? 12 : tmv.tm_hour % 12;
    (void)snprintf(buf, buf_len, "%d:%02d %s", hour12, tmv.tm_min, tmv.tm_hour < 12 ? "AM" : "PM");
}

static void notice_now(const char *text, uint32_t stamp)
{
    (void)snprintf(s.notice, sizeof(s.notice), "%s", text);
    s.notice_until_ms = stamp + UI_SHELL_NOTICE_MS;
}

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

static const char *status_line(void)
{
    switch (s.screen) {
    case ZV_UI_SCREEN_MENU:
        return "MENU";
    case ZV_UI_SCREEN_NETWORK:
        return "NETWORK";
    case ZV_UI_SCREEN_VOLUME:
        return "VOLUME";
    case ZV_UI_SCREEN_CONFIRM:
        return "RESTART?";
    case ZV_UI_SCREEN_HOME:
    default:
        return "READY";
    }
}

static const char *detail_line(void)
{
    if (s.screen == ZV_UI_SCREEN_NETWORK) {
        return !s.voice_mode ? "NOT CHECKED" :
            (s.online ? "AUTHENTICATED ONLINE" : "GATEWAY OFFLINE");
    }
    if (s.notice[0] != '\0') {
        return s.notice;
    }
    if (s.screen == ZV_UI_SCREEN_MENU) {
        return "TAP A ROW - HOLD HOME TO EXIT";
    }
    return "ZENTARI VOICE PUCK";
}

static esp_err_t paint(uint32_t stamp)
{
    /* Cache displayed values, never pointers into mutable clock/notice buffers.
     * The voice renderer owns the panel between settings visits, so every new
     * visit explicitly invalidates this snapshot. */
    static struct {
        bool valid, muted, nous, online, battery_ok;
        zv_ui_screen_t screen;
        uint8_t focus, level, bars, battery_percent;
        char clock[16], detail[48];
    } last;
    zv_ui_canvas_t canvas = {.ctx = NULL, .fill_rect = canvas_fill, .draw_row = canvas_row};
    zv_ui_chrome_t chrome;
    zv_face_frame_t face;
    zv_status_t err;

    if ((int32_t)(stamp - s.notice_until_ms) >= 0 && s.notice_until_ms != 0u) {
        s.notice[0] = '\0';
        s.notice_until_ms = 0u;
    }
    memset(&chrome, 0, sizeof(chrome));
    chrome.brand_text = "ZENTARI";
    chrome.show_connectivity = true;
    chrome.use_zenti_head = true;
    chrome.show_home = true;
    chrome.show_volume = true;
    chrome.volume = s.volume;
    chrome.screen = s.screen;
    chrome.menu_open = s.screen == ZV_UI_SCREEN_MENU || s.screen == ZV_UI_SCREEN_OPTIONS;
    chrome.menu_focus = s.screen == ZV_UI_SCREEN_MENU ? s.focus : (uint8_t)ZV_UI_NO_FOCUS;
    chrome.menu_count = s.screen == ZV_UI_SCREEN_MENU ? (size_t)SHELL_ITEM_COUNT : 0u;
    for (size_t i = 0; i < (size_t)SHELL_ITEM_COUNT; i++) {
        chrome.menu_items[i] = k_items[i];
    }
    /* Keep the current mute flag visible after the transient notice expires. */
    chrome.menu_items[SHELL_ITEM_MUTE] = zv_volume_is_muted(&s.volume) ? "MUTE: ON" : "MUTE: OFF";

    if (s.screen == ZV_UI_SCREEN_OPTIONS) {
        chrome.menu_count=4; chrome.menu_focus=s.focus;
        chrome.menu_items[0]=zv_ui_nous_selected()?"ZENTI":"ZENTI: SELECTED";
        chrome.menu_items[1]=zv_ui_nous_selected()?"NOUS: SELECTED":"NOUS GIRL";
        chrome.menu_items[2]="NEW CONVERSATION"; chrome.menu_items[3]="BACK";
        chrome.status_text="AVATAR / CHAT";
    }
    format_clock(s.clock, sizeof(s.clock));
    /* Before SNTP the slot would otherwise flicker empty every repaint; showing
     * the real time the moment it exists is better than a permanent placeholder. */
    chrome.clock_text = s.clock[0] != '\0' ? s.clock : "TIME NOT SET";
    chrome.status_text = status_line();
    chrome.detail_text = detail_line();
    chrome.signal_bars = s.voice_mode ? s.link_bars : 0u;
    chrome.secure_link = s.voice_mode && s.online;
    chrome.gateway_online = chrome.secure_link;
    fill_battery_chrome(&chrome);

    bool full = !last.valid || s.force_paint || last.screen != s.screen ||
        last.focus != s.focus || last.level != s.volume.level ||
        last.muted != s.volume.muted || last.nous != zv_ui_nous_selected() ||
        strcmp(last.detail, chrome.detail_text) != 0;
    bool clock_changed = !last.valid || strcmp(last.clock, chrome.clock_text) != 0;
    bool signal_changed = !last.valid || last.bars != chrome.signal_bars;
    bool status_changed = !last.valid || last.online != chrome.gateway_online ||
        last.battery_ok != chrome.battery_ok || last.battery_percent != chrome.battery_percent;
    if (!full && !clock_changed && !signal_changed && !status_changed) return ESP_OK;

    /* Repainting under a moving finger only smears; the gesture ends in a repaint
     * anyway, so the drag preview is allowed but the periodic refresh waits. */
    err = zv_avatar_set_state(&s.model, ZV_AVATAR_IDLE, stamp);
    if (err != ZV_OK) {
        return ESP_FAIL;
    }
    err = zv_avatar_frame(&s.model, stamp, NULL, 0u, &face);
    if (err != ZV_OK) {
        return ESP_FAIL;
    }
    if (full) {
        err = zv_ui_paint_frame(&canvas, &face, ZV_AVATAR_IDLE, &chrome);
    } else {
        /* Connection/header changes never clear or repaint the open menu.
         * Reuse the real painter through a clipped board canvas; no extra
         * framebuffer or new allocation is needed. */
        const settings_clip_t regions[] = {{96, 0, 258, 80}, {52, 56, 96, 84},
                                          {258, 58, 314, 92},
                                          {48, chrome.menu_open ? 300 : 284, 312, 316}};
        /* The online dot also appears in the status chip below the header.
         * Menu/Options cover its upper half, so preserve their pixels too. */
        const bool changed[] = {clock_changed, signal_changed, status_changed,
                               last.online != chrome.gateway_online};
        for (size_t i = 0; i < 4u && err == ZV_OK; ++i) {
            if (!changed[i]) continue;
            settings_clip_t region = regions[i];
            /* Menu/Options begin at y=84 and cover the battery percentage.
             * Even their unchanged top border must stay outside LCD writes. */
            if (chrome.menu_open && i < 3u && region.y1 > 84) region.y1 = 84;
            canvas.ctx = &region;
            err = zv_ui_paint_frame(&canvas, &face, ZV_AVATAR_IDLE, &chrome);
        }
    }
    if (err != ZV_OK) {
        s.force_paint = true;
        return zv_to_esp(err);
    }
    last.valid = true; last.screen = s.screen; last.focus = s.focus;
    last.level = s.volume.level; last.muted = s.volume.muted;
    last.nous = zv_ui_nous_selected(); last.online = chrome.gateway_online;
    last.bars = chrome.signal_bars; last.battery_ok = chrome.battery_ok;
    last.battery_percent = chrome.battery_percent;
    (void)snprintf(last.clock, sizeof(last.clock), "%s", chrome.clock_text);
    (void)snprintf(last.detail, sizeof(last.detail), "%s", chrome.detail_text);
    s.force_paint = false;
    return ESP_OK;
}

/** Maps a completed touch gesture onto the current screen. */
static void on_release(zv_gesture_t gesture, int16_t x, int16_t y, uint32_t stamp)
{
    if (gesture == ZV_GESTURE_NONE) {
        return;
    }
    if (s.screen == ZV_UI_SCREEN_NETWORK) {
        if ((gesture == ZV_GESTURE_TAP || gesture == ZV_GESTURE_LONG_PRESS) &&
            (zv_ui_volume_accept_hit(x, y) || zv_ui_home_hit(x, y))) {
            s.screen = ZV_UI_SCREEN_MENU;
        }
        return;
    }
    /* Accepting volume is harmless and must also work for a deliberate hold.
     * Resolve it before the generic long-press filter. */
    if (s.screen == ZV_UI_SCREEN_VOLUME && zv_ui_volume_accept_hit(x, y) &&
        (gesture == ZV_GESTURE_TAP || gesture == ZV_GESTURE_LONG_PRESS)) {
        s.screen = ZV_UI_SCREEN_MENU;
        return;
    }
    if (s.screen == ZV_UI_SCREEN_OPTIONS && gesture == ZV_GESTURE_TAP) {
        uint8_t option=0;
        if (zv_ui_menu_hit(x,y,4,&option)) {
            if (option < 2u) { zv_ui_select_avatar(option==1u?"nous-girl":"zenti"); settings_save_avatar(); s.screen=ZV_UI_SCREEN_HOME; }
            else if (option==2u) { s_reset_conversation=true; s.screen=ZV_UI_SCREEN_HOME; }
            else s.screen=ZV_UI_SCREEN_MENU;
        } else s.screen=ZV_UI_SCREEN_MENU;
        return;
    }
    uint8_t hit = 0u;
    bool mute_control = (s.screen == ZV_UI_SCREEN_HOME && zv_ui_volume_hit(x, y)) ||
        (s.screen == ZV_UI_SCREEN_VOLUME && zv_ui_volume_mute_hit(x, y)) ||
        (s.screen == ZV_UI_SCREEN_MENU && zv_ui_menu_hit(x, y, (uint8_t)SHELL_ITEM_COUNT, &hit) &&
         hit == (uint8_t)SHELL_ITEM_MUTE);
    if (gesture == ZV_GESTURE_LONG_PRESS && mute_control) {
        gesture = ZV_GESTURE_TAP; /* harmless toggle, once on release */
    }
    /* Long press on the home corner leaves the shell so an acceptance run can
     * finish and report; a short tap never does. */
    if (gesture == ZV_GESTURE_LONG_PRESS && zv_ui_home_hit(x, y)) {
        ESP_LOGI(TAG, "long press on home: leaving shell");
        if (s.voice_mode) s.screen = ZV_UI_SCREEN_HOME;
        else s.ready = false;
        return;
    }
    if (gesture != ZV_GESTURE_TAP && gesture != ZV_GESTURE_DRAG) {
        return;
    }

    if (s.screen == ZV_UI_SCREEN_HOME && zv_ui_volume_hit(x, y)) {
        bool muted = zv_volume_toggle_mute(&s.volume);
        ESP_LOGI(TAG, "mute %s (level remembered at %u)", zv_mute_state_name(muted),
                 (unsigned)zv_volume_level(&s.volume));
        notice_now(muted ? "MUTED" : "UNMUTED", stamp);
        return;
    }

    switch (s.screen) {
    case ZV_UI_SCREEN_HOME:
        if (zv_ui_home_hit(x, y)) {
            s.screen = ZV_UI_SCREEN_MENU;
        }
        break;
    case ZV_UI_SCREEN_MENU: {
        uint8_t index = 0u;
        if (!zv_ui_menu_hit(x, y, (uint8_t)SHELL_ITEM_COUNT, &index)) {
            s.screen = ZV_UI_SCREEN_HOME;
            break;
        }
        s.focus = index;
        switch ((shell_item_t)index) {
        case SHELL_ITEM_NETWORK:
            s.screen = ZV_UI_SCREEN_NETWORK;
            break;
        case SHELL_ITEM_VOLUME:
            s.screen = ZV_UI_SCREEN_VOLUME;
            break;
        case SHELL_ITEM_MUTE:
            (void)zv_volume_toggle_mute(&s.volume);
            s.focus = zv_volume_is_muted(&s.volume) ? (uint8_t)SHELL_ITEM_MUTE : (uint8_t)ZV_UI_NO_FOCUS;
            notice_now(zv_volume_is_muted(&s.volume) ? "MUTED" : "UNMUTED", stamp);
            break;
        case SHELL_ITEM_OPTIONS:
            s.screen=ZV_UI_SCREEN_OPTIONS; s.focus=ZV_UI_NO_FOCUS; break;
        case SHELL_ITEM_RESTART:
            s.screen = ZV_UI_SCREEN_CONFIRM;
            break;
        default:
            break;
        }
        break;
    }
    case ZV_UI_SCREEN_VOLUME:
        if (zv_ui_volume_mute_hit(x, y)) {
            (void)zv_volume_toggle_mute(&s.volume);
            notice_now(zv_volume_is_muted(&s.volume) ? "MUTED" : "UNMUTED", stamp);
        } else if (zv_ui_volume_accept_hit(x, y) || zv_ui_home_hit(x, y)) {
            s.screen = ZV_UI_SCREEN_MENU;
        }
        break;
    case ZV_UI_SCREEN_CONFIRM:
        if (zv_ui_confirm_hit_yes(x, y)) {
            ESP_LOGW(TAG, "restart confirmed by operator");
            esp_restart();
        } else {
            s.screen = ZV_UI_SCREEN_MENU;
        }
        break;
    default:
        break;
    }
}

/** Live drag preview on the volume slider. */
static bool on_press(int16_t x, int16_t y)
{
    uint8_t previous_level = zv_volume_level(&s.volume);
    if (s.screen == ZV_UI_SCREEN_VOLUME && zv_ui_volume_bar_hit(x, y)) {
        (void)zv_volume_set_level(&s.volume, zv_ui_volume_from_x(x));
    }
    return previous_level != zv_volume_level(&s.volume);
}

static void settings_init(void)
{
    if (s.ready) return;
    apply_timezone();
    memset(&s, 0, sizeof(s));
    s.ready = true;
    s.force_paint = true;
    if (nvs_flash_init()!=ESP_OK) ESP_LOGW(TAG,"avatar storage initialization unavailable");
    nvs_handle_t handle; char avatar[24]="zenti"; size_t length=sizeof(avatar);
    if (nvs_open("zv_ui",NVS_READONLY,&handle)==ESP_OK) {
        if (nvs_get_str(handle,"avatar",avatar,&length)!=ESP_OK) (void)snprintf(avatar,sizeof(avatar),"zenti");
        nvs_close(handle);
    }
    zv_ui_select_avatar(avatar);
    s.screen = ZV_UI_SCREEN_HOME;
    settings_load_volume();
    zv_touch_init(&s.touch);
    zv_repaint_init(&s.repaint, CONFIG_ZV_UI_REPAINT_PERIOD_MS);
    (void)zv_avatar_init(&s.model, now_ms(), 0xA5A5A5A5u);
}

void zv_ui_settings_init(void) { settings_init(); }
uint8_t zv_ui_voice_volume(void) { settings_init(); return zv_volume_effective(&s.volume); }
zv_volume_t zv_ui_voice_volume_state(void) { settings_init(); return s.volume; }
bool zv_ui_settings_active(void) { return s.ready && s.voice_mode && s.screen != ZV_UI_SCREEN_HOME; }

static esp_err_t settings_open_screen(uint8_t bars, bool online, zv_ui_screen_t screen)
{
    settings_init();
    s.voice_mode = true; s.online = online; s.link_bars = bars;
    s.screen = screen; s.focus = ZV_UI_NO_FOCUS;
    s.force_paint = true;
    s.ignore_contact = true; /* Consume the opening button's release. */
    zv_touch_init(&s.touch);
    esp_err_t err = paint(now_ms());
    /* The opening frame already satisfies the periodic refresh. Without this,
     * the first released contact immediately triggers a second full repaint. */
    if (err == ESP_OK) zv_repaint_mark(&s.repaint, now_ms());
    return err;
}

esp_err_t zv_ui_settings_open(uint8_t bars, bool online)
{ return settings_open_screen(bars, online, ZV_UI_SCREEN_MENU); }
esp_err_t zv_ui_settings_open_volume(uint8_t bars, bool online)
{ return settings_open_screen(bars, online, ZV_UI_SCREEN_VOLUME); }

esp_err_t zv_ui_settings_poll(bool touched, uint16_t x, uint16_t y, uint32_t stamp)
{
    if (!zv_ui_settings_active()) return ESP_OK;
    if (s.ignore_contact) {
        if (!touched) s.ignore_contact = false;
        return ESP_OK;
    }
    bool dirty = false, pressed = s.touch.pressed;
    if (zv_touch_push(&s.touch, stamp, x, y, touched)) {
        if (touched) dirty = on_press(x, y);
        else if (!touched && pressed) {
            on_release(zv_touch_classify(&s.touch), s.touch.end_x, s.touch.end_y, stamp);
            settings_save_volume();
            dirty = true;
        }
    }
    if (!s.touch.pressed && zv_repaint_due(&s.repaint, stamp)) {
        zv_repaint_mark(&s.repaint, stamp); dirty = true;
    }
    return dirty && zv_ui_settings_active() ? paint(stamp) : ESP_OK;
}

esp_err_t zv_ui_settings_resume(uint8_t bars, bool online)
{
    bool changed = s.link_bars != bars || s.online != online;
    s.link_bars = bars; s.online = online;
    return (changed || s.force_paint) && zv_ui_settings_active() ? paint(now_ms()) : ESP_OK;
}

esp_err_t zv_ui_shell(void)
{
    ESP_RETURN_ON_ERROR(zv_board_lcd_init(), TAG, "shell LCD init failed");
    ESP_RETURN_ON_ERROR(zv_board_touch_init(), TAG, "shell touch init failed");
    ESP_RETURN_ON_ERROR(zv_board_battery_init(), TAG, "shell battery init failed");
    ESP_RETURN_ON_ERROR(zv_board_backlight_set(80), TAG, "shell backlight failed");

    s.ready = false;
    settings_init();

    ESP_LOGI(TAG, "interactive shell up: repaint every %d ms, TZ=%s, volume %u",
             (int)CONFIG_ZV_UI_REPAINT_PERIOD_MS, CONFIG_ZV_UI_TZ, (unsigned)zv_volume_level(&s.volume));

    while (s.ready) {
        uint32_t stamp = now_ms();
        bool touched = false;
        uint16_t raw_x = 0u, raw_y = 0u;
        ESP_RETURN_ON_ERROR(zv_board_touch_poll(&touched, &raw_x, &raw_y), TAG, "shell touch poll failed");

        bool dirty = false;
        bool was_pressed = s.touch.pressed;
        if (zv_touch_push(&s.touch, stamp, (int16_t)raw_x, (int16_t)raw_y, touched)) {
            if (touched && !was_pressed) {
                dirty = on_press((int16_t)raw_x, (int16_t)raw_y);
                /* Only slider changes need a press repaint. Drawing a whole
                 * frame on ACCEPT blocks polling and can miss a quick release. */
            } else if (!touched && was_pressed) {
                zv_gesture_t gesture = zv_touch_classify(&s.touch);
                zv_ui_screen_t previous = s.screen;
                on_release(gesture, s.touch.end_x, s.touch.end_y, stamp);
                settings_save_volume();
                ESP_LOGI(TAG, "touch %s at x=%d y=%d: %s -> %s", zv_gesture_name(gesture),
                         (int)s.touch.end_x, (int)s.touch.end_y, zv_ui_screen_name(previous),
                         zv_ui_screen_name(s.screen));
                dirty = true;
            }
        }

        /* The clock is the only thing that changes without input, so it is what
         * this cadence exists for. Input-driven repaints deliberately do not
         * reset it: the guarantee is that the clock is at most one period stale,
         * not that the last frame happened one period ago. */
        if (!s.touch.pressed && zv_repaint_due(&s.repaint, stamp)) {
            zv_repaint_mark(&s.repaint, stamp);
            dirty = true;
        }

        if (dirty) {
            ESP_RETURN_ON_ERROR(paint(stamp), TAG, "shell paint failed");
        }
        vTaskDelay(pdMS_TO_TICKS(UI_SHELL_POLL_MS));
    }

    ESP_LOGI(TAG, "interactive shell exited; platform remains gated");
    return ESP_OK;
}

#else /* CONFIG_ZV_UI_SHELL_MODE */

esp_err_t zv_ui_shell(void) { return ESP_ERR_NOT_SUPPORTED; }
void zv_ui_settings_init(void) {}
uint8_t zv_ui_voice_volume(void) { return 0u; }
zv_volume_t zv_ui_voice_volume_state(void) { return (zv_volume_t){0}; }
bool zv_ui_settings_active(void) { return false; }
esp_err_t zv_ui_settings_open(uint8_t bars, bool online) { (void)bars; (void)online; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t zv_ui_settings_open_volume(uint8_t bars, bool online) { (void)bars; (void)online; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t zv_ui_settings_resume(uint8_t bars, bool online) { (void)bars; (void)online; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t zv_ui_settings_poll(bool touched, uint16_t x, uint16_t y, uint32_t stamp)
{ (void)touched; (void)x; (void)y; (void)stamp; return ESP_ERR_NOT_SUPPORTED; }

#endif /* CONFIG_ZV_UI_SHELL_MODE */
