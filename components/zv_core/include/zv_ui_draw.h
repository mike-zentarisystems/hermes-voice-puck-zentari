/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 *
 * Portable, allocation-free UI painting for the eight avatar states.
 * Hardware-free: canvas callbacks receive fills and optional RGB565 rows, so the
 * same scene builder drives the ST77916 on device and the host test recorder.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "zv_avatar_model.h"
#include "zv_status.h"
#include "zv_volume.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZV_UI_W 360
#define ZV_UI_H 360
#define ZV_UI_WAVE_BARS 40
#define ZV_UI_MAX_MENU 5
#define ZV_UI_FONT_W 5
#define ZV_UI_FONT_H 7
#define ZV_UI_FONT_SCALE 2
/** Menu hit target around the visible 36px control at (234,329). */
#define ZV_UI_HOME_X0 214
#define ZV_UI_HOME_Y0 309
#define ZV_UI_HOME_X1 254
#define ZV_UI_HOME_Y1 349

/** Microphone hit target around the visible 36px control at (126,329). */
#define ZV_UI_MIC_X0 106
#define ZV_UI_MIC_Y0 309
#define ZV_UI_MIC_X1 146
#define ZV_UI_MIC_Y1 349

/** Shared predicates keep voice and menu touch regions aligned with the UI. */
static inline bool zv_ui_touch_is_mic(uint16_t x, uint16_t y)
{
    if (x < ZV_UI_MIC_X0 || x >= ZV_UI_MIC_X1 || y < ZV_UI_MIC_Y0 || y >= ZV_UI_MIC_Y1) return false;
    int32_t dx = (int32_t)x - 126, dy = (int32_t)y - 329;
    return dx * dx + dy * dy <= 400;
}

static inline bool zv_ui_touch_is_home(uint16_t x, uint16_t y)
{
    if (x < ZV_UI_HOME_X0 || x >= ZV_UI_HOME_X1 || y < ZV_UI_HOME_Y0 || y >= ZV_UI_HOME_Y1) return false;
    int32_t dx = (int32_t)x - 234, dy = (int32_t)y - 329;
    return dx * dx + dy * dy <= 400;
}

/** The avatar face is the push-to-talk button; bottom controls stay separate. */
static inline bool zv_ui_touch_is_voice_function(uint16_t x, uint16_t y)
{
    int32_t dx = (int32_t)x - 180, dy = (int32_t)y - 180;
    return dx * dx + dy * dy <= 108 * 108;
}

/** Dedicated cancel target, centred between volume and menu on the round panel. */
static inline bool zv_ui_cancel_hit(uint16_t x, uint16_t y)
{
    return x >= 158u && x < 202u && y >= 307u && y < 359u;
}

static inline bool zv_ui_touch_is_volume(uint16_t x, uint16_t y)
{
    int32_t dx=(int32_t)x-126, dy=(int32_t)y-329;
    int32_t panel_x=(int32_t)x-180, panel_y=(int32_t)y-180;
    return dx*dx+dy*dy <= 22*22 && panel_x*panel_x+panel_y*panel_y < 180*180;
}

/** Dedicated approval controls used by the bounded UI acceptance flow.
 * Inset so both buttons sit inside the round display. */
#define ZV_UI_APPROVE_X0 60
#define ZV_UI_APPROVE_Y0 262
#define ZV_UI_APPROVE_X1 174
#define ZV_UI_APPROVE_Y1 308
#define ZV_UI_DENY_X0 186
#define ZV_UI_DENY_Y0 262
#define ZV_UI_DENY_X1 300
#define ZV_UI_DENY_Y1 308

/** Battery level thresholds for color escalation (from PR #16). */
#define ZV_UI_BATTERY_LOW_PCT 20u
#define ZV_UI_BATTERY_CRITICAL_PCT 10u

/*
 * Hit geometry for the interactive shell. These are shared on purpose: the
 * painter draws from them and the firmware hit-tests against them, so a layout
 * change cannot move a target away from the pixels the operator can see. Every
 * menu row is ZV_UI_MENU_ITEM_H tall and the row text baseline sits eight pixels below its top.
 */
#define ZV_UI_MENU_X0 64
#define ZV_UI_MENU_X1 296
#define ZV_UI_MENU_Y0 104
#define ZV_UI_MENU_ITEM_H 39

/** Shell finger-pad tolerance around bottom controls; right/bottom are exclusive.
 * These hit areas do not change the reference painter or voice mic geometry. */
#define ZV_UI_SHELL_HOME_TOUCH_X0 198
#define ZV_UI_SHELL_HOME_TOUCH_X1 272
#define ZV_UI_SHELL_VOLUME_TOUCH_X0 88
#define ZV_UI_SHELL_VOLUME_TOUCH_X1 164
#define ZV_UI_SHELL_BOTTOM_TOUCH_Y0 300
#define ZV_UI_SHELL_BOTTOM_TOUCH_Y1 ZV_UI_H

/** Bottom-left volume / mute indicator, which is also the mute tap target. */
#define ZV_UI_VOLUME_X0 108
#define ZV_UI_VOLUME_Y0 315
#define ZV_UI_VOLUME_X1 144
#define ZV_UI_VOLUME_Y1 343

/** Explicit mute toggle above the volume slider. */
#define ZV_UI_VOLUME_MUTE_X0 116
#define ZV_UI_VOLUME_MUTE_Y0 214
#define ZV_UI_VOLUME_MUTE_X1 244
#define ZV_UI_VOLUME_MUTE_Y1 238

/** Volume slider on the volume screen; a drag maps x across this span. */
#define ZV_UI_VOLUME_BAR_X0 48
#define ZV_UI_VOLUME_BAR_Y0 244
#define ZV_UI_VOLUME_BAR_X1 312
#define ZV_UI_VOLUME_BAR_Y1 264

/** Visible volume acceptance button below the slider. */
#define ZV_UI_VOLUME_ACCEPT_X0 116
#define ZV_UI_VOLUME_ACCEPT_Y0 272
#define ZV_UI_VOLUME_ACCEPT_X1 244
#define ZV_UI_VOLUME_ACCEPT_Y1 298

/** The two choices on the restart confirmation screen. */
#define ZV_UI_CONFIRM_YES_X0 44
#define ZV_UI_CONFIRM_YES_Y0 246
#define ZV_UI_CONFIRM_YES_X1 164
#define ZV_UI_CONFIRM_YES_Y1 290
#define ZV_UI_CONFIRM_NO_X0 196
#define ZV_UI_CONFIRM_NO_Y0 246
#define ZV_UI_CONFIRM_NO_X1 316
#define ZV_UI_CONFIRM_NO_Y1 290

/** menu_focus value meaning no row is highlighted. */
#define ZV_UI_NO_FOCUS 0xFFu

/** Which shell screen the frame belongs to. */
typedef enum {
    ZV_UI_SCREEN_HOME = 0,
    ZV_UI_SCREEN_MENU,
    ZV_UI_SCREEN_VOLUME,
    ZV_UI_SCREEN_CONFIRM,
    ZV_UI_SCREEN_OPTIONS,
    ZV_UI_SCREEN_NETWORK,
} zv_ui_screen_t;

/** Immediate-mode canvas. fill_rect must clip or reject out-of-bounds boxes. */
typedef struct {
    void *ctx;
    zv_status_t (*fill_rect)(void *ctx, int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color);
    /** Optional RGB565 row transport; pixels are valid only during this call. */
    zv_status_t (*draw_row)(void *ctx, int16_t x, int16_t y, uint16_t width, const uint16_t *pixels);
} zv_ui_canvas_t;

/** Non-face chrome layered on top of one avatar frame. */
typedef struct {
    const char *status_text; /**< NULL means zv_avatar_state_name(state) */
    const char *brand_text;  /**< Optional compact product label at top-left */
    const char *date_text;   /**< Optional centered date (e.g. "WED, SEP 30"); NULL/empty omits it */
    const char *clock_text;  /**< Optional centered clock (e.g. "7:52 PM"); NULL/empty omits it */
    const char *notice;      /**< Optional top banner; NULL omits it */
    const char *detail_text; /**< Optional secondary line below the primary state */
    bool show_connectivity;
    uint8_t signal_bars;     /**< 0..3 Wi-Fi strength bars */
    bool secure_link;        /**< Authenticated TLS session indicator */
    bool gateway_online;     /**< Gateway reachability indicator */
    bool battery_ok;         /**< Battery reading is valid; false paints an unknown glyph */
    uint8_t battery_percent; /**< 0..100; clamped */
    bool use_zenti_head;     /**< Paint the smooth reference mascot instead of the legacy face */
    bool show_home;          /**< Paint the menu affordance at the bottom */
    bool show_mic;           /**< Paint bottom-left microphone/privacy cue */
    bool show_cancel;        /**< Active voice turn: explicit discard/stop control */
    bool mic_muted;          /**< True paints the privacy-disabled/muted cue */
    bool menu_open;
    const char *menu_items[ZV_UI_MAX_MENU];
    size_t menu_count;
    uint8_t menu_focus;      /**< Highlighted row, or ZV_UI_NO_FOCUS */
    zv_ui_screen_t screen;   /**< Which shell screen this frame represents */
    bool show_volume;        /**< Paint the bottom-left volume / mute indicator */
    zv_volume_t volume;      /**< Indicator state; the painter derives bars from it */
    bool show_wave;
    int16_t wave[ZV_UI_WAVE_BARS]; /**< Signed permille about the centre line */
    size_t wave_len;
} zv_ui_chrome_t;

void zv_ui_select_avatar(const char *id);
bool zv_ui_nous_selected(void);

uint16_t zv_rgb_to_rgb565(uint8_t r, uint8_t g, uint8_t b);
uint16_t zv_rgb888_to_rgb565(uint32_t rgb888);
/** Blend rgb888 toward black by intensity_permille (0 = black, 1000 = full). */
uint16_t zv_rgb888_to_rgb565_scaled(uint32_t rgb888, uint16_t intensity_permille);

zv_status_t zv_ui_paint_text(zv_ui_canvas_t *canvas, int16_t x, int16_t y, const char *text, uint16_t color,
                             uint8_t scale);
/** Advances by (strlen * (ZV_UI_FONT_W + 1) * scale). */
int16_t zv_ui_text_width(const char *text, uint8_t scale);

/**
 * Paints the generated 8-bit Zenti head sprite centred at (cx, cy).
 * Each sprite pixel becomes scale x scale screen pixels. tint_rgb888 remaps
 * the helmet whites toward the state colour; NULL keeps the brand palette.
 */
zv_status_t zv_ui_paint_zenti_head(zv_ui_canvas_t *canvas, int16_t cx, int16_t cy, uint8_t scale,
                                   uint32_t tint_rgb888);

/**
 * Paints background, state ring, eyes, mouth, waveform, clock, status, notice,
 * and optional menu for one avatar face frame. Pure with respect to time: the
 * caller supplies the face frame already advanced by zv_avatar_frame().
 */
zv_status_t zv_ui_paint_frame(zv_ui_canvas_t *canvas, const zv_face_frame_t *face, const zv_avatar_state_t state,
                              const zv_ui_chrome_t *chrome);

/*
 * Hit testing. Kept beside the painter so the firmware shell and the host tests
 * resolve taps through exactly the geometry that gets drawn.
 */
bool zv_ui_home_hit(int16_t x, int16_t y);
bool zv_ui_volume_hit(int16_t x, int16_t y);

/** Maps a tap onto a menu row. index_limit caps the rows the caller exposes. */
bool zv_ui_menu_hit(int16_t x, int16_t y, uint8_t index_limit, uint8_t *index_out);

bool zv_ui_confirm_hit_yes(int16_t x, int16_t y);
bool zv_ui_confirm_hit_no(int16_t x, int16_t y);

/** True when the coordinate is inside the slider, muted plate excluded. */
bool zv_ui_volume_bar_hit(int16_t x, int16_t y);
bool zv_ui_volume_mute_hit(int16_t x, int16_t y);
bool zv_ui_volume_accept_hit(int16_t x, int16_t y);

/** Projects a slider x coordinate onto 0..ZV_VOLUME_MAX, clamped at both ends. */
uint8_t zv_ui_volume_from_x(int16_t x);

const char *zv_ui_screen_name(zv_ui_screen_t screen);

#ifdef __cplusplus
}
#endif
