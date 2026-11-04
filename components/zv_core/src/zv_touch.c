/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 */
#include "zv_touch.h"

#include <string.h>

#define ZV_TOUCH_DEFAULT_LONG_PRESS_MS 500u
#define ZV_TOUCH_DEFAULT_SWIPE_MAX_MS 600u
#define ZV_TOUCH_DEFAULT_MOVE_PX 12u

static uint32_t abs_diff(int16_t a, int16_t b)
{
    int32_t delta = (int32_t)a - (int32_t)b;
    return (uint32_t)(delta < 0 ? -delta : delta);
}

void zv_touch_init(zv_touch_t *touch)
{
    if (touch == NULL) {
        return;
    }
    memset(touch, 0, sizeof(*touch));
    touch->long_press_ms = ZV_TOUCH_DEFAULT_LONG_PRESS_MS;
    touch->swipe_max_ms = ZV_TOUCH_DEFAULT_SWIPE_MAX_MS;
    touch->move_threshold_px = ZV_TOUCH_DEFAULT_MOVE_PX;
    touch->result = ZV_GESTURE_NONE;
}

void zv_touch_configure(zv_touch_t *touch, uint32_t long_press_ms, uint32_t swipe_max_ms, uint16_t move_threshold_px)
{
    if (touch == NULL) {
        return;
    }
    touch->long_press_ms = long_press_ms;
    touch->swipe_max_ms = swipe_max_ms;
    touch->move_threshold_px = move_threshold_px;
}

void zv_touch_reset(zv_touch_t *touch)
{
    uint32_t long_press_ms = ZV_TOUCH_DEFAULT_LONG_PRESS_MS;
    uint32_t swipe_max_ms = ZV_TOUCH_DEFAULT_SWIPE_MAX_MS;
    uint16_t move_px = ZV_TOUCH_DEFAULT_MOVE_PX;
    if (touch == NULL) {
        return;
    }
    long_press_ms = touch->long_press_ms;
    swipe_max_ms = touch->swipe_max_ms;
    move_px = touch->move_threshold_px;
    zv_touch_init(touch);
    touch->long_press_ms = long_press_ms;
    touch->swipe_max_ms = swipe_max_ms;
    touch->move_threshold_px = move_px;
}

static void ring_push(zv_touch_t *touch, uint32_t now_ms, int16_t x, int16_t y)
{
    touch->ring[touch->head].t_ms = now_ms;
    touch->ring[touch->head].x = x;
    touch->ring[touch->head].y = y;
    touch->head = (touch->head + 1u) % ZV_TOUCH_SAMPLES;
    if (touch->count < ZV_TOUCH_SAMPLES) {
        touch->count++;
    }
}

size_t zv_touch_sample_count(const zv_touch_t *touch) { return touch == NULL ? 0u : touch->count; }

/* A zero-duration contact carries no intent signal, so it is never a gesture. */
static zv_gesture_t decide(const zv_touch_t *touch)
{
    uint32_t duration = touch->release_ms - touch->start_ms;
    if (duration == 0u) {
        return ZV_GESTURE_NONE;
    }
    uint32_t adx = abs_diff(touch->end_x, touch->start_x);
    uint32_t ady = abs_diff(touch->end_y, touch->start_y);
    bool moved = adx >= touch->move_threshold_px || ady >= touch->move_threshold_px;

    if (!moved) {
        return duration >= touch->long_press_ms ? ZV_GESTURE_LONG_PRESS : ZV_GESTURE_TAP;
    }
    if (duration > touch->swipe_max_ms) {
        return ZV_GESTURE_DRAG; /* slow travel is a drag, not a flick */
    }
    /* Axis dominance is strict on X so a 45-degree diagonal resolves to exactly
     * one axis, deterministically, instead of both. */
    if (adx >= ady) {
        return touch->end_x > touch->start_x ? ZV_GESTURE_SWIPE_RIGHT : ZV_GESTURE_SWIPE_LEFT;
    }
    return touch->end_y > touch->start_y ? ZV_GESTURE_SWIPE_DOWN : ZV_GESTURE_SWIPE_UP;
}

bool zv_touch_push(zv_touch_t *touch, uint32_t now_ms, int16_t x, int16_t y, bool pressed)
{
    if (touch == NULL) {
        return false;
    }
    /* Strictly monotonic: equal or backwards timestamps are refused outright so
     * a stuck or rewound clock cannot synthesise a gesture. Keyed off the sample
     * count rather than last_ms, because a legitimate first sample at t=0 leaves
     * last_ms at 0 and must not open a gap. */
    if (touch->count > 0u && now_ms <= touch->last_ms) {
        return false;
    }

    /* Board coordinates are undefined once contact is released. Preserve the
     * last pressed point, rather than interpreting an idle (0,0) as movement. */
    if (!pressed && touch->pressed) {
        x = touch->end_x;
        y = touch->end_y;
    }
    ring_push(touch, now_ms, x, y);

    if (pressed && !touch->pressed) {
        touch->pressed = true;
        touch->start_ms = now_ms;
        touch->start_x = x;
        touch->start_y = y;
        touch->has_result = false;
        touch->result = ZV_GESTURE_NONE;
    }
    if (pressed) {
        touch->end_x = x;
        touch->end_y = y;
    } else if (!pressed && touch->pressed) {
        touch->pressed = false;
        touch->release_ms = now_ms;
        touch->end_x = x;
        touch->end_y = y;
        touch->result = decide(touch);
        touch->has_result = true;
    }

    touch->last_ms = now_ms;
    return true;
}

zv_gesture_t zv_touch_classify(const zv_touch_t *touch)
{
    if (touch == NULL || !touch->has_result) {
        return ZV_GESTURE_NONE;
    }
    return touch->result;
}

uint32_t zv_touch_held_ms(const zv_touch_t *touch, uint32_t now_ms)
{
    if (touch == NULL || !touch->pressed || now_ms < touch->start_ms) {
        return 0u;
    }
    return now_ms - touch->start_ms;
}

const char *zv_gesture_name(zv_gesture_t gesture)
{
    switch (gesture) {
    case ZV_GESTURE_TAP:
        return "tap";
    case ZV_GESTURE_LONG_PRESS:
        return "long_press";
    case ZV_GESTURE_DRAG:
        return "drag";
    case ZV_GESTURE_SWIPE_LEFT:
        return "swipe_left";
    case ZV_GESTURE_SWIPE_RIGHT:
        return "swipe_right";
    case ZV_GESTURE_SWIPE_UP:
        return "swipe_up";
    case ZV_GESTURE_SWIPE_DOWN:
        return "swipe_down";
    case ZV_GESTURE_NONE:
    default:
        return "none";
    }
}

void zv_touch_menu_init(zv_touch_menu_t *menu, uint8_t item_count)
{
    if (menu == NULL) {
        return;
    }
    memset(menu, 0, sizeof(*menu));
    menu->item_count = item_count > ZV_TOUCH_MENU_MAX ? ZV_TOUCH_MENU_MAX : item_count;
    menu->focus = 0u;
}

void zv_touch_menu_geometry(zv_touch_menu_t *menu, int16_t x0, int16_t x1, int16_t y0, int16_t item_h)
{
    if (menu == NULL) {
        return;
    }
    menu->x0 = x0;
    menu->x1 = x1;
    menu->y0 = y0;
    menu->item_h = item_h;
}

zv_status_t zv_touch_menu_move_focus(zv_touch_menu_t *menu, zv_gesture_t gesture)
{
    if (menu == NULL || menu->item_count == 0u) {
        return ZV_ERR_INVALID_STATE;
    }
    int step = 0;
    if (gesture == ZV_GESTURE_SWIPE_DOWN) {
        step = 1;
    } else if (gesture == ZV_GESTURE_SWIPE_UP) {
        step = -1;
    } else {
        return ZV_ERR_UNSUPPORTED;
    }
    int focus = (int)menu->focus + step;
    /* Wrap: a menu is a loop, not a list that dead-ends. */
    if (focus < 0) {
        focus = (int)menu->item_count - 1;
    }
    if (focus >= (int)menu->item_count) {
        focus = 0;
    }
    menu->focus = (uint8_t)focus;
    return ZV_OK;
}

bool zv_touch_menu_hit(const zv_touch_menu_t *menu, int16_t x, int16_t y, uint8_t *index_out)
{
    if (menu == NULL || index_out == NULL || menu->item_count == 0u || menu->item_h <= 0) {
        return false;
    }
    if (x < menu->x0 || x > menu->x1 || y < menu->y0) {
        return false;
    }
    int32_t offset = (int32_t)y - (int32_t)menu->y0;
    int32_t index = offset / menu->item_h;
    if (index < 0 || index >= (int32_t)menu->item_count) {
        return false;
    }
    *index_out = (uint8_t)index;
    return true;
}

zv_status_t zv_touch_menu_activate(zv_touch_menu_t *menu, int16_t x, int16_t y, uint8_t *activated_out)
{
    if (menu == NULL || activated_out == NULL || menu->item_count == 0u) {
        return ZV_ERR_INVALID_STATE;
    }
    uint8_t hit = 0u;
    if (!zv_touch_menu_hit(menu, x, y, &hit)) {
        return ZV_ERR_INVALID_ARG;
    }
    /* The hit region becomes the device's own focus. The wire never supplies an
     * index; a caller wanting remote selection must go through the signed
     * mutating path, where this exact value is what gets approved. */
    menu->focus = hit;
    *activated_out = hit;
    return ZV_OK;
}