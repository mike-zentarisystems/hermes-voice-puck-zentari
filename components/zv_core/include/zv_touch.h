/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 *
 * Portable touch gesture recognition and menu focus. No ESP-IDF, no allocation,
 * no I/O: the caller supplies timestamps and coordinates. Same discipline as
 * zv_link_supervisor, which never performs I/O.
 *
 * The CST816 also has a hardware gesture engine at register 0x01, but this
 * recognizer exists because the register yields discrete, positionless gestures.
 * Continuous control (a drag on a volume slider) needs the coordinates.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "zv_status.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Fixed-capacity sample history. Overflow evicts the oldest entry. */
#define ZV_TOUCH_SAMPLES 12u
#define ZV_TOUCH_MENU_MAX 5u

typedef enum {
    ZV_GESTURE_NONE = 0,
    ZV_GESTURE_TAP,
    ZV_GESTURE_LONG_PRESS,
    ZV_GESTURE_DRAG,
    ZV_GESTURE_SWIPE_LEFT,
    ZV_GESTURE_SWIPE_RIGHT,
    ZV_GESTURE_SWIPE_UP,
    ZV_GESTURE_SWIPE_DOWN,
    ZV_GESTURE_COUNT,
} zv_gesture_t;

typedef struct {
    uint32_t t_ms;
    int16_t x;
    int16_t y;
} zv_touch_sample_t;

typedef struct {
    zv_touch_sample_t ring[ZV_TOUCH_SAMPLES];
    size_t count;
    size_t head; /**< next write index */

    bool pressed;
    bool has_result;
    uint32_t start_ms;
    uint32_t release_ms;
    int16_t start_x;
    int16_t start_y;
    int16_t end_x;
    int16_t end_y;

    uint32_t last_ms;
    uint32_t long_press_ms;
    uint32_t swipe_max_ms;
    uint16_t move_threshold_px;

    zv_gesture_t result;
} zv_touch_t;

/**
 * Menu focus owned by the device. `focus` is always derived from local input:
 * a gateway-supplied index is never honoured, because a confused or compromised
 * sender must not be able to choose which item gets activated.
 */
typedef struct {
    bool menu_open;
    uint8_t item_count;
    uint8_t focus;
    int16_t x0;
    int16_t x1;
    int16_t y0;
    int16_t item_h;
} zv_touch_menu_t;

void zv_touch_init(zv_touch_t *touch);

/** Sensible defaults: long press 500 ms, swipe within 600 ms, 12 px jitter floor. */
void zv_touch_configure(zv_touch_t *touch, uint32_t long_press_ms, uint32_t swipe_max_ms, uint16_t move_threshold_px);

void zv_touch_reset(zv_touch_t *touch);

/**
 * Feeds one contact sample. Returns false when the timestamp is not strictly
 * greater than the previous one, in which case no state changes. A gesture is
 * decided on release, never while the finger is still down.
 */
/** Coordinates are used only while pressed; release retains the last pressed point. */
bool zv_touch_push(zv_touch_t *touch, uint32_t now_ms, int16_t x, int16_t y, bool pressed);

/** Last decided gesture. ZV_GESTURE_NONE before any release. */
zv_gesture_t zv_touch_classify(const zv_touch_t *touch);

/** Milliseconds the current contact has been held; 0 when not pressed. */
uint32_t zv_touch_held_ms(const zv_touch_t *touch, uint32_t now_ms);

const char *zv_gesture_name(zv_gesture_t gesture);

/** Total recorded samples, capped at ZV_TOUCH_SAMPLES (oldest evicted). */
size_t zv_touch_sample_count(const zv_touch_t *touch);

void zv_touch_menu_init(zv_touch_menu_t *menu, uint8_t item_count);
void zv_touch_menu_geometry(zv_touch_menu_t *menu, int16_t x0, int16_t x1, int16_t y0, int16_t item_h);

/** Maps a horizontal/vertical swipe onto the device-owned focus. */
zv_status_t zv_touch_menu_move_focus(zv_touch_menu_t *menu, zv_gesture_t gesture);

/** Hit-tests a coordinate against the locally laid-out items. */
bool zv_touch_menu_hit(const zv_touch_menu_t *menu, int16_t x, int16_t y, uint8_t *index_out);

/**
 * Resolves a tap to the locally focused item. Returns the device-derived index
 * in *activated_out and never reads an index from the wire.
 */
zv_status_t zv_touch_menu_activate(zv_touch_menu_t *menu, int16_t x, int16_t y, uint8_t *activated_out);

#ifdef __cplusplus
}
#endif