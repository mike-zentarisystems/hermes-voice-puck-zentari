/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>

#include "zv_test.h"
#include "zv_ui_draw.h"
#include "zv_zenti_sprite.h"

#define MAX_RECORDED 4096

typedef struct {
    size_t count;
    bool overflow;
    bool saw_ring_color;
    uint16_t last_color;
    int16_t min_x0, min_y0, max_x1, max_y1;
    bool bounds_init;
    int amber_count; /* fills in the exact low-battery amber */
    int red_count;   /* fills in the exact error red */
    int wake_count;  /* fills fully inside the booting wake-dot band */
} rec_t;

static zv_status_t rec_fill(void *ctx, int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color)
{
    rec_t *r = (rec_t *)ctx;
    if (r->count >= MAX_RECORDED) {
        r->overflow = true;
        return ZV_OK;
    }
    r->count++;
    r->last_color = color;
    if (color == zv_rgb888_to_rgb565(ZV_COLOR_APPROVAL)) {
        r->amber_count++;
    }
    if (color == zv_rgb888_to_rgb565(ZV_COLOR_ERROR)) {
        r->red_count++;
    }
    if (x0 >= 155 && x1 <= 205 && y0 >= 258 && y1 <= 292) {
        r->wake_count++; /* booting wake dots paint here; head outline may too */
    }
    if (x0 < 0 || y0 < 0 || x1 > ZV_UI_W || y1 > ZV_UI_H || x1 <= x0 || y1 <= y0) {
        return ZV_ERR_INVALID_STATE;
    }
    if (!r->bounds_init) {
        r->min_x0 = x0;
        r->min_y0 = y0;
        r->max_x1 = x1;
        r->max_y1 = y1;
        r->bounds_init = true;
    } else {
        if (x0 < r->min_x0) r->min_x0 = x0;
        if (y0 < r->min_y0) r->min_y0 = y0;
        if (x1 > r->max_x1) r->max_x1 = x1;
        if (y1 > r->max_y1) r->max_y1 = y1;
    }
    return ZV_OK;
}

static zv_status_t rec_row(void *ctx, int16_t x, int16_t y, uint16_t width, const uint16_t *pixels)
{
    if (pixels == NULL) return ZV_ERR_INVALID_ARG;
    return rec_fill(ctx, x, y, (int16_t)(x + width), (int16_t)(y + 1), 0u);
}

static void test_rgb565(void)
{
    ZV_CHECK_EQ(zv_rgb_to_rgb565(0xFF, 0x00, 0x00), 0xF800);
    ZV_CHECK_EQ(zv_rgb_to_rgb565(0x00, 0xFF, 0x00), 0x07E0);
    ZV_CHECK_EQ(zv_rgb_to_rgb565(0x00, 0x00, 0xFF), 0x001F);
    ZV_CHECK_EQ(zv_rgb888_to_rgb565(0x000000u), 0x0000);
    ZV_CHECK(zv_rgb888_to_rgb565_scaled(0xFFFFFFu, 0) == 0x0000);
    ZV_CHECK(zv_rgb888_to_rgb565_scaled(0xFFFFFFu, 1000) == 0xFFFF);
}

static void test_text(void)
{
    rec_t rec;
    memset(&rec, 0, sizeof(rec));
    zv_ui_canvas_t canvas = {.ctx = &rec, .fill_rect = rec_fill, .draw_row = rec_row};
    ZV_CHECK_EQ(zv_ui_paint_text(&canvas, 10, 10, "LISTENING", 0xFFFF, 2), ZV_OK);
    ZV_CHECK(rec.count > 0);
    ZV_CHECK_EQ(zv_ui_text_width("AB", 2), (int16_t)(2 * 6 * 2));
    ZV_CHECK_EQ(zv_ui_paint_text(&canvas, 0, 0, NULL, 0, 2), ZV_ERR_INVALID_ARG);
}

static void test_all_eight_states(void)
{
    zv_avatar_model_t model;
    ZV_CHECK_EQ(zv_avatar_init(&model, 0, 42u), ZV_OK);

    for (int s = 0; s < (int)ZV_AVATAR_STATE_COUNT; s++) {
        rec_t rec;
        memset(&rec, 0, sizeof(rec));
        zv_ui_canvas_t canvas = {.ctx = &rec, .fill_rect = rec_fill, .draw_row = rec_row};
        ZV_CHECK_EQ(zv_avatar_set_state(&model, (zv_avatar_state_t)s, 0u), ZV_OK);
        zv_face_frame_t face;
        ZV_CHECK_EQ(zv_avatar_frame(&model, 100u, NULL, 0, &face), ZV_OK);

        zv_ui_chrome_t chrome;
        memset(&chrome, 0, sizeof(chrome));
        chrome.show_wave = (s == (int)ZV_AVATAR_SPEAKING || s == (int)ZV_AVATAR_LISTENING);
        chrome.wave_len = chrome.show_wave ? ZV_UI_WAVE_BARS : 0;
        for (int i = 0; i < ZV_UI_WAVE_BARS; i++) {
            chrome.wave[i] = (int16_t)((i % 5) * 200 - 400);
        }

        ZV_CHECK_EQ(zv_ui_paint_frame(&canvas, &face, (zv_avatar_state_t)s, &chrome), ZV_OK);
        ZV_CHECK(!rec.overflow);
        ZV_CHECK(rec.count > 20); /* background + ring + eyes + mouth + status */
        ZV_CHECK(rec.min_x0 >= 0);
        ZV_CHECK(rec.max_x1 <= ZV_UI_W);
        ZV_CHECK(rec.min_y0 >= 0);
        ZV_CHECK(rec.max_y1 <= ZV_UI_H);
    }
}

static void test_ring_colors(void)
{
    struct {
        zv_avatar_state_t state;
        uint32_t rgb;
    } cases[] = {
        {ZV_AVATAR_IDLE, ZV_COLOR_ZENTARI_BLUE},
        {ZV_AVATAR_SUCCESS, ZV_COLOR_SUCCESS},
        {ZV_AVATAR_ERROR, ZV_COLOR_ERROR},
        {ZV_AVATAR_APPROVAL, ZV_COLOR_APPROVAL},
    };
    zv_avatar_model_t model;
    ZV_CHECK_EQ(zv_avatar_init(&model, 0, 1u), ZV_OK);
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        rec_t rec;
        memset(&rec, 0, sizeof(rec));
        zv_ui_canvas_t canvas = {.ctx = &rec, .fill_rect = rec_fill, .draw_row = rec_row};
        ZV_CHECK_EQ(zv_avatar_set_state(&model, cases[i].state, 0u), ZV_OK);
        zv_face_frame_t face;
        ZV_CHECK_EQ(zv_avatar_frame(&model, 10u, NULL, 1000u, &face), ZV_OK);
        ZV_CHECK_EQ(face.ring_rgb, cases[i].rgb);
        ZV_CHECK_EQ(zv_ui_paint_frame(&canvas, &face, cases[i].state, NULL), ZV_OK);
        /* Ring segments are the only high-intensity fills of that colour after bg. */
        ZV_CHECK(rec.count > 72);
    }
}

static void test_menu_overlay(void)
{
    rec_t rec;
    memset(&rec, 0, sizeof(rec));
    zv_ui_canvas_t canvas = {.ctx = &rec, .fill_rect = rec_fill, .draw_row = rec_row};
    zv_avatar_model_t model;
    ZV_CHECK_EQ(zv_avatar_init(&model, 0, 9u), ZV_OK);
    zv_face_frame_t face;
    ZV_CHECK_EQ(zv_avatar_frame(&model, 0u, NULL, 0u, &face), ZV_OK);
    zv_ui_chrome_t chrome;
    memset(&chrome, 0, sizeof(chrome));
    chrome.menu_open = true;
    chrome.menu_count = 2;
    chrome.menu_items[0] = "NETWORK";
    chrome.menu_items[1] = "VOLUME";
    chrome.status_text = "MENU";
    size_t before;
    {
        rec_t base;
        memset(&base, 0, sizeof(base));
        zv_ui_canvas_t b = {.ctx = &base, .fill_rect = rec_fill, .draw_row = rec_row};
        ZV_CHECK_EQ(zv_ui_paint_frame(&b, &face, ZV_AVATAR_IDLE, NULL), ZV_OK);
        before = base.count;
    }
    ZV_CHECK_EQ(zv_ui_paint_frame(&canvas, &face, ZV_AVATAR_IDLE, &chrome), ZV_OK);
    ZV_CHECK(rec.count > before);
}

static void test_v2_interface_chrome(void)
{
    zv_avatar_model_t model;
    ZV_CHECK_EQ(zv_avatar_init(&model, 0, 11u), ZV_OK);
    zv_face_frame_t face;
    ZV_CHECK_EQ(zv_avatar_frame(&model, 0u, NULL, 0u, &face), ZV_OK);

    rec_t base;
    memset(&base, 0, sizeof(base));
    zv_ui_canvas_t base_canvas = {.ctx = &base, .fill_rect = rec_fill, .draw_row = rec_row};
    ZV_CHECK_EQ(zv_ui_paint_frame(&base_canvas, &face, ZV_AVATAR_IDLE, NULL), ZV_OK);

    rec_t rich;
    memset(&rich, 0, sizeof(rich));
    zv_ui_canvas_t rich_canvas = {.ctx = &rich, .fill_rect = rec_fill, .draw_row = rec_row};
    zv_ui_chrome_t chrome;
    memset(&chrome, 0, sizeof(chrome));
    chrome.brand_text = "ZENTARI";
    chrome.status_text = "ONLINE";
    chrome.detail_text = "SECURE SESSION";
    chrome.notice = "READY";
    chrome.show_connectivity = true;
    chrome.signal_bars = 9u; /* renderer clamps malformed telemetry */
    chrome.battery_ok = true;
    chrome.battery_percent = 250u; /* clamped to 100 */
    chrome.secure_link = true;
    chrome.gateway_online = true;
    ZV_CHECK_EQ(zv_ui_paint_frame(&rich_canvas, &face, ZV_AVATAR_IDLE, &chrome), ZV_OK);
    ZV_CHECK(!rich.overflow);
    ZV_CHECK(rich.count > base.count);
    ZV_CHECK(rich.min_x0 >= 0);
    ZV_CHECK(rich.max_x1 <= ZV_UI_W);
    ZV_CHECK(rich.min_y0 >= 0);
    ZV_CHECK(rich.max_y1 <= ZV_UI_H);
    /* The banded core keeps the full rich frame below the DMA-call budget. */
    ZV_CHECK(rich.count < 900u);
}

static void test_speaking_mouth_follows_level(void)
{
    zv_avatar_model_t model;
    ZV_CHECK_EQ(zv_avatar_init(&model, 0, 3u), ZV_OK);
    ZV_CHECK_EQ(zv_avatar_set_state(&model, ZV_AVATAR_SPEAKING, 0u), ZV_OK);
    zv_mouth_shape_t quiet = {.open = 0, .width = 400, .round = 0};
    zv_mouth_shape_t loud = {.open = 900, .width = 500, .round = 0};
    zv_face_frame_t a, b;
    ZV_CHECK_EQ(zv_avatar_frame(&model, 10u, &quiet, 0u, &a), ZV_OK);
    ZV_CHECK_EQ(zv_avatar_frame(&model, 20u, &loud, 900u, &b), ZV_OK);
    ZV_CHECK(b.mouth_open > a.mouth_open);
}

static void test_zenti_head_sprite(void)
{
    rec_t rec;
    memset(&rec, 0, sizeof(rec));
    zv_ui_canvas_t canvas = {.ctx = &rec, .fill_rect = rec_fill, .draw_row = rec_row};
    ZV_CHECK_EQ(zv_ui_paint_zenti_head(&canvas, 180, 180, 3u, 0u), ZV_OK);
    /* Physical-LCD palette contract: helmet stays white, visor stays dark, and
       the primary accent remains the same Zentari blue used by the state ring. */
    ZV_CHECK_EQ(zv_zenti_palette[2], zv_rgb888_to_rgb565(0xFFFFFFu));
    ZV_CHECK_EQ(zv_zenti_palette[5], zv_rgb888_to_rgb565(0x1F75FEu));
    ZV_CHECK_EQ(zv_zenti_palette[7], zv_rgb888_to_rgb565(0x061A3Au));

    ZV_CHECK(rec.count > 50u);
    ZV_CHECK(rec.count < 400u); /* DMA-call ceiling for the sprite alone */
    ZV_CHECK(!rec.overflow);
    ZV_CHECK(rec.min_x0 >= 0);
    ZV_CHECK(rec.max_x1 <= ZV_UI_W);
    ZV_CHECK(rec.min_y0 >= 0);
    ZV_CHECK(rec.max_y1 <= ZV_UI_H);

    zv_avatar_model_t model;
    ZV_CHECK_EQ(zv_avatar_init(&model, 0, 9u), ZV_OK);
    zv_face_frame_t face;
    ZV_CHECK_EQ(zv_avatar_frame(&model, 10u, NULL, 0u, &face), ZV_OK);
    rec_t full;
    memset(&full, 0, sizeof(full));
    zv_ui_canvas_t full_canvas = {.ctx = &full, .fill_rect = rec_fill, .draw_row = rec_row};
    zv_ui_chrome_t chrome;
    memset(&chrome, 0, sizeof(chrome));
    chrome.use_zenti_head = true;
    chrome.show_connectivity = true;
    chrome.signal_bars = 2u;
    chrome.battery_ok = true;
    chrome.battery_percent = 80u;
    chrome.date_text = "WED, SEP 30";
    chrome.clock_text = "7:52 PM";
    chrome.status_text = "IDLE";
    chrome.show_home = true;
    chrome.show_mic = true;
    ZV_CHECK_EQ(zv_ui_paint_frame(&full_canvas, &face, ZV_AVATAR_IDLE, &chrome), ZV_OK);
    ZV_CHECK(!full.overflow);
    ZV_CHECK(full.count < 900u);

    rec_t muted;
    memset(&muted, 0, sizeof(muted));
    zv_ui_canvas_t muted_canvas = {.ctx = &muted, .fill_rect = rec_fill, .draw_row = rec_row};
    chrome.mic_muted = true;
    ZV_CHECK_EQ(zv_ui_paint_frame(&muted_canvas, &face, ZV_AVATAR_IDLE, &chrome), ZV_OK);
    ZV_CHECK(!muted.overflow);
    ZV_CHECK(muted.count > full.count); /* privacy slash adds fills */
    ZV_CHECK(muted.count < 900u);
}
static void paint_battery_case(zv_avatar_model_t *model, uint8_t pct, rec_t *rec)
{
    zv_face_frame_t face;
    ZV_CHECK_EQ(zv_avatar_frame(model, 0u, NULL, 0u, &face), ZV_OK);
    zv_ui_chrome_t chrome;
    memset(&chrome, 0, sizeof(chrome));
    chrome.use_zenti_head = true;
    chrome.show_connectivity = true;
    chrome.battery_ok = true;
    chrome.battery_percent = pct;
    zv_ui_canvas_t canvas = {.ctx = rec, .fill_rect = rec_fill, .draw_row = rec_row};
    ZV_CHECK_EQ(zv_ui_paint_frame(&canvas, &face, ZV_AVATAR_IDLE, &chrome), ZV_OK);
}

static void test_low_battery_escalation(void)
{
    zv_avatar_model_t model;
    ZV_CHECK_EQ(zv_avatar_init(&model, 0, 23u), ZV_OK);

    rec_t healthy, low, critical;
    memset(&healthy, 0, sizeof(healthy));
    memset(&low, 0, sizeof(low));
    memset(&critical, 0, sizeof(critical));
    paint_battery_case(&model, 25u, &healthy);
    paint_battery_case(&model, ZV_UI_BATTERY_LOW_PCT, &low);
    paint_battery_case(&model, ZV_UI_BATTERY_CRITICAL_PCT, &critical);

    /* Healthy keeps the ring-derived fill; low turns amber; critical turns red. */
    ZV_CHECK(healthy.amber_count == 0 && healthy.red_count == 0);
    ZV_CHECK(low.amber_count >= 1 && low.red_count == 0);
    /* Critical: fill plus the two warning-tick segments, exactly three reds. */
    ZV_CHECK(critical.red_count == 3 && critical.amber_count == 0);
    ZV_CHECK(low.min_x0 >= 0 && low.max_x1 <= ZV_UI_W);
    ZV_CHECK(low.min_y0 >= 0 && low.max_y1 <= ZV_UI_H);
    ZV_CHECK(critical.min_x0 >= 0 && critical.max_x1 <= ZV_UI_W);
    ZV_CHECK(critical.min_y0 >= 0 && critical.max_y1 <= ZV_UI_H);
}

static void test_booting_screen(void)
{
    zv_avatar_model_t model;
    ZV_CHECK_EQ(zv_avatar_init(&model, 0, 31u), ZV_OK);
    zv_face_frame_t face;
    ZV_CHECK_EQ(zv_avatar_frame(&model, 0u, NULL, 0u, &face), ZV_OK);

    zv_ui_chrome_t chrome;
    memset(&chrome, 0, sizeof(chrome));
    chrome.use_zenti_head = true;
    chrome.show_home = true;
    chrome.show_mic = true;
    chrome.show_connectivity = true;
    chrome.signal_bars = 0u;
    chrome.battery_ok = true;
    chrome.battery_percent = 80u;
    chrome.date_text = "DATE NOT SET";
    chrome.clock_text = "TIME NOT SET";

    rec_t base, boot, lower;
    memset(&base, 0, sizeof(base));
    memset(&boot, 0, sizeof(boot));
    memset(&lower, 0, sizeof(lower));

    chrome.status_text = "IDLE";
    zv_ui_canvas_t base_canvas = {.ctx = &base, .fill_rect = rec_fill, .draw_row = rec_row};
    ZV_CHECK_EQ(zv_ui_paint_frame(&base_canvas, &face, ZV_AVATAR_IDLE, &chrome), ZV_OK);

    chrome.status_text = "STARTING";
    zv_ui_canvas_t boot_canvas = {.ctx = &boot, .fill_rect = rec_fill, .draw_row = rec_row};
    ZV_CHECK_EQ(zv_ui_paint_frame(&boot_canvas, &face, ZV_AVATAR_IDLE, &chrome), ZV_OK);

    /* A lowercase caller form must paint the identical screen. */
    chrome.status_text = "starting";
    zv_ui_canvas_t lower_canvas = {.ctx = &lower, .fill_rect = rec_fill, .draw_row = rec_row};
    ZV_CHECK_EQ(zv_ui_paint_frame(&lower_canvas, &face, ZV_AVATAR_IDLE, &chrome), ZV_OK);

    /* Exactly the three waking dots appear in the wake band. */
    ZV_CHECK(boot.wake_count == base.wake_count + 3);
    ZV_CHECK(lower.wake_count == boot.wake_count);
    /* Boot is the densest chrome: fallback date/time strings + mic + home
       measured at 925 fills, so this tripwire sits just above that. */
    ZV_CHECK(boot.count < 1000u);
    ZV_CHECK(boot.min_x0 >= 0 && boot.max_x1 <= ZV_UI_W);
    ZV_CHECK(boot.min_y0 >= 0 && boot.max_y1 <= ZV_UI_H);
}

static void test_link_cues_accept_lowercase(void)
{
    /* zv_link_state_name() returns lowercase names; the painter must still
       draw the semantic cues (regression: strcmp needed exact uppercase). */
    zv_avatar_model_t model;
    ZV_CHECK_EQ(zv_avatar_init(&model, 0, 37u), ZV_OK);
    zv_face_frame_t face;
    ZV_CHECK_EQ(zv_avatar_frame(&model, 0u, NULL, 0u, &face), ZV_OK);

    zv_ui_chrome_t chrome;
    memset(&chrome, 0, sizeof(chrome));
    chrome.use_zenti_head = true;

    rec_t base, upper_c, lower_c, upper_o, lower_o;
    memset(&base, 0, sizeof(base));
    memset(&upper_c, 0, sizeof(upper_c));
    memset(&lower_c, 0, sizeof(lower_c));
    memset(&upper_o, 0, sizeof(upper_o));
    memset(&lower_o, 0, sizeof(lower_o));

    /* Same avatar state everywhere so only status_text and its cue differ. */
    struct {
        const char *status;
        rec_t *rec;
    } cases[] = {
        {"IDLE", &base},
        {"CONNECTING", &upper_c},
        {"connecting", &lower_c},
        {"OFFLINE", &upper_o},
        {"offline", &lower_o},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        chrome.status_text = cases[i].status;
        zv_ui_canvas_t canvas = {.ctx = cases[i].rec, .fill_rect = rec_fill, .draw_row = rec_row};
        ZV_CHECK_EQ(zv_ui_paint_frame(&canvas, &face, ZV_AVATAR_IDLE, &chrome), ZV_OK);
    }

    ZV_CHECK(upper_c.wake_count == base.wake_count + 3); /* three cue dots */
    ZV_CHECK(lower_c.wake_count == upper_c.wake_count);  /* lowercase identical */
    ZV_CHECK(upper_o.wake_count == base.wake_count + 1); /* one cue bar */
    ZV_CHECK(lower_o.wake_count == upper_o.wake_count);
}


static void test_zenti_state_effects_and_approval_panel(void)
{
    zv_avatar_model_t model;
    ZV_CHECK_EQ(zv_avatar_init(&model, 0u, 17u), ZV_OK);

    for (int s = (int)ZV_AVATAR_LISTENING; s <= (int)ZV_AVATAR_APPROVAL; s++) {
        rec_t rec;
        memset(&rec, 0, sizeof(rec));
        zv_ui_canvas_t canvas = {.ctx = &rec, .fill_rect = rec_fill, .draw_row = rec_row};
        ZV_CHECK_EQ(zv_avatar_set_state(&model, (zv_avatar_state_t)s, 0u), ZV_OK);

        zv_face_frame_t face;
        ZV_CHECK_EQ(zv_avatar_frame(&model, 150u, NULL, 700u, &face), ZV_OK);

        zv_ui_chrome_t chrome;
        memset(&chrome, 0, sizeof(chrome));
        chrome.use_zenti_head = true;
        chrome.status_text = zv_avatar_state_name((zv_avatar_state_t)s);

        ZV_CHECK_EQ(zv_ui_paint_frame(&canvas, &face, (zv_avatar_state_t)s, &chrome), ZV_OK);
        ZV_CHECK(!rec.overflow);
        ZV_CHECK(rec.bounds_init);
        ZV_CHECK(rec.min_x0 >= 0);
        ZV_CHECK(rec.max_x1 <= ZV_UI_W);
        ZV_CHECK(rec.min_y0 >= 0);
        ZV_CHECK(rec.max_y1 <= ZV_UI_H);
    }

    /* Link-state semantic cues reuse avatar states but must also remain in bounds. */
    rec_t link;
    memset(&link, 0, sizeof(link));
    zv_ui_canvas_t link_canvas = {.ctx = &link, .fill_rect = rec_fill, .draw_row = rec_row};
    ZV_CHECK_EQ(zv_avatar_set_state(&model, ZV_AVATAR_THINKING, 0u), ZV_OK);
    zv_face_frame_t link_face;
    ZV_CHECK_EQ(zv_avatar_frame(&model, 250u, NULL, 0u, &link_face), ZV_OK);
    zv_ui_chrome_t link_chrome;
    memset(&link_chrome, 0, sizeof(link_chrome));
    link_chrome.use_zenti_head = true;
    link_chrome.status_text = "CONNECTING";
    ZV_CHECK_EQ(zv_ui_paint_frame(&link_canvas, &link_face, ZV_AVATAR_THINKING, &link_chrome), ZV_OK);
    ZV_CHECK(!link.overflow);
}

static uint16_t reference_rows[ZV_UI_W * ZV_UI_H];
static uint16_t reference_fills[ZV_UI_W * ZV_UI_H];

static zv_status_t framebuffer_fill(void *ctx, int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color)
{
    if (x0 < 0 || y0 < 0 || x1 > ZV_UI_W || y1 > ZV_UI_H || x1 <= x0 || y1 <= y0) return ZV_ERR_INVALID_ARG;
    uint16_t *pixels = ctx;
    for (int16_t y = y0; y < y1; y++)
        for (int16_t x = x0; x < x1; x++) pixels[y * ZV_UI_W + x] = color;
    return ZV_OK;
}

static zv_status_t framebuffer_row(void *ctx, int16_t x, int16_t y, uint16_t width, const uint16_t *pixels)
{
    if (pixels == NULL || width == 0u || x < 0 || y < 0 || y >= ZV_UI_H || x + width > ZV_UI_W) return ZV_ERR_INVALID_ARG;
    memcpy((uint16_t *)ctx + y * ZV_UI_W + x, pixels, (size_t)width * sizeof(uint16_t));
    return ZV_OK;
}

static void test_mouth_changes_never_replace_the_surrounding_portrait(void)
{
    const char *avatars[] = {"zenti", "nous-girl"};
    zv_ui_chrome_t chrome = {0}; chrome.use_zenti_head = true;
    zv_face_frame_t face = {0}; face.eye_open = 1000u;
    zv_ui_canvas_t a = {.ctx=reference_rows, .fill_rect=framebuffer_fill, .draw_row=framebuffer_row};
    zv_ui_canvas_t b = {.ctx=reference_fills, .fill_rect=framebuffer_fill, .draw_row=framebuffer_row};
    for (size_t avatar=0; avatar<2; ++avatar) {
        zv_ui_select_avatar(avatars[avatar]);
        face.mouth_open=0;
        ZV_CHECK_EQ(zv_ui_paint_frame(&a,&face,ZV_AVATAR_SPEAKING,&chrome),ZV_OK);
        for (uint16_t opening=300; opening<=900; opening+=300) {
            face.mouth_open=opening;
            ZV_CHECK_EQ(zv_ui_paint_frame(&b,&face,ZV_AVATAR_SPEAKING,&chrome),ZV_OK);
            unsigned inside=0, outside=0;
            for (int y=0; y<ZV_UI_H; ++y) for (int x=0; x<ZV_UI_W; ++x) {
                if (reference_rows[y*ZV_UI_W+x]==reference_fills[y*ZV_UI_W+x]) continue;
                bool mouth = x>=164 && x<197 && y>=(avatar ? 208 : 230) && y<(avatar ? 234 : 253);
                if (mouth) ++inside; else ++outside;
            }
            ZV_CHECK_EQ(outside,0u);
            ZV_CHECK(inside>0u); /* each amplitude step has a distinct aperture */
            memcpy(reference_rows,reference_fills,sizeof(reference_rows));
        }
    }
    zv_ui_select_avatar("zenti");
}

static void test_nous_opens_below_the_upper_lip_and_preserves_other_expressions(void)
{
    zv_ui_select_avatar("nous-girl");
    zv_ui_chrome_t chrome={0};chrome.use_zenti_head=true;
    zv_face_frame_t face={0};face.eye_open=1000u;
    zv_ui_canvas_t quiet={.ctx=reference_rows,.fill_rect=framebuffer_fill,.draw_row=framebuffer_row};
    zv_ui_canvas_t loud={.ctx=reference_fills,.fill_rect=framebuffer_fill,.draw_row=framebuffer_row};
    ZV_CHECK_EQ(zv_ui_paint_frame(&quiet,&face,ZV_AVATAR_SPEAKING,&chrome),ZV_OK);
    face.mouth_open=1000u;
    ZV_CHECK_EQ(zv_ui_paint_frame(&loud,&face,ZV_AVATAR_SPEAKING,&chrome),ZV_OK);
    unsigned upper_changes=0,lower_changes=0;
    for (int y=208;y<217;++y) for (int x=164;x<197;++x)
        if (reference_rows[y*ZV_UI_W+x]!=reference_fills[y*ZV_UI_W+x]) ++upper_changes;
    for (int y=226;y<234;++y) for (int x=170;x<191;++x)
        if (reference_rows[y*ZV_UI_W+x]!=reference_fills[y*ZV_UI_W+x]) ++lower_changes;
    ZV_CHECK_EQ(upper_changes,0u);
    ZV_CHECK(lower_changes>0u); /* The lower lip moves instead of being painted over. */
    const zv_avatar_state_t states[]={ZV_AVATAR_IDLE,ZV_AVATAR_LISTENING,ZV_AVATAR_SUCCESS};
    for (size_t i=0;i<sizeof(states)/sizeof(states[0]);++i) {
        face.mouth_open=0;
        ZV_CHECK_EQ(zv_ui_paint_frame(&quiet,&face,states[i],&chrome),ZV_OK);
        face.mouth_open=1000;
        ZV_CHECK_EQ(zv_ui_paint_frame(&loud,&face,states[i],&chrome),ZV_OK);
        ZV_CHECK(memcmp(reference_rows,reference_fills,sizeof(reference_rows))==0);
    }
    zv_ui_select_avatar("zenti");
}

static void test_home_volume_percent_and_dedicated_cancel_pixels(void)
{
    zv_ui_canvas_t actual={.ctx=reference_rows,.fill_rect=framebuffer_fill,.draw_row=framebuffer_row};
    zv_ui_canvas_t expected={.ctx=reference_fills,.fill_rect=framebuffer_fill,.draw_row=framebuffer_row};
    zv_ui_chrome_t chrome={0};chrome.use_zenti_head=true;chrome.show_volume=true;
    zv_face_frame_t face={0};face.eye_open=1000;
    const uint8_t levels[]={0,40,100};const char *labels[]={"0%","40%","100%"};
    uint16_t ink=zv_rgb888_to_rgb565_scaled(0xD9F2FFu,860u);
    for (size_t i=0;i<3;++i) {
        chrome.volume.level=levels[i];
        ZV_CHECK_EQ(zv_ui_paint_frame(&actual,&face,ZV_AVATAR_IDLE,&chrome),ZV_OK);
        memset(reference_fills,0,sizeof(reference_fills));
        int16_t left=(int16_t)(126-zv_ui_text_width(labels[i],1u)/2);
        ZV_CHECK_EQ(zv_ui_paint_text(&expected,left,334,labels[i],ink,1u),ZV_OK);
        unsigned mismatched=0,lit=0;
        for (int y=334;y<341;++y) for (int x=left;x<left+zv_ui_text_width(labels[i],1u);++x) {
            if (reference_fills[y*360+x]==ink) {
                ++lit;if(reference_rows[y*360+x]!=ink) ++mismatched;
            }
        }
        ZV_CHECK(lit>0);ZV_CHECK_EQ(mismatched,0u);
    }
    chrome.show_cancel=true;
    ZV_CHECK_EQ(zv_ui_paint_frame(&actual,&face,ZV_AVATAR_LISTENING,&chrome),ZV_OK);
    ZV_CHECK_EQ(reference_rows[329*360+180],zv_rgb888_to_rgb565(ZV_COLOR_ERROR));
    chrome.show_cancel=false;
    ZV_CHECK_EQ(zv_ui_paint_frame(&expected,&face,ZV_AVATAR_LISTENING,&chrome),ZV_OK);
    unsigned changed_inside=0,changed_outside=0;
    for(uint16_t y=0;y<360;++y) for(uint16_t x=0;x<360;++x) {
        if(reference_rows[y*360+x]!=reference_fills[y*360+x]) {
            if(zv_ui_cancel_hit(x,y)) ++changed_inside;else ++changed_outside;
        }
    }
    ZV_CHECK(changed_inside>0);ZV_CHECK_EQ(changed_outside,0u);
}

static zv_status_t failed_row(void *ctx, int16_t x, int16_t y, uint16_t width, const uint16_t *pixels)
{
    (void)ctx; (void)x; (void)y; (void)width; (void)pixels;
    return ZV_ERR_HARDWARE_GATE;
}

static void test_reference_transport_parity(void)
{
    zv_avatar_model_t model;
    ZV_CHECK_EQ(zv_avatar_init(&model, 0u, 17u), ZV_OK);
    zv_ui_chrome_t chrome;
    memset(&chrome, 0, sizeof(chrome));
    chrome.use_zenti_head = true; chrome.show_home = true; chrome.show_mic = true;
    chrome.show_connectivity = true; chrome.battery_ok = true; chrome.battery_percent = 85u;
    chrome.clock_text = "11:59 AM"; chrome.date_text = "TUE, OCT 6";
    chrome.notice = "DELETE ALL KEYS";
    zv_ui_canvas_t rows = {.ctx = reference_rows, .fill_rect = framebuffer_fill, .draw_row = framebuffer_row};
    zv_ui_canvas_t fills = {.ctx = reference_fills, .fill_rect = framebuffer_fill};
    for (int s = 0; s < (int)ZV_AVATAR_STATE_COUNT; s++) {
        zv_face_frame_t face;
        ZV_CHECK_EQ(zv_avatar_set_state(&model, (zv_avatar_state_t)s, 0u), ZV_OK);
        ZV_CHECK_EQ(zv_avatar_frame(&model, 100u, NULL, 500u, &face), ZV_OK);
        ZV_CHECK_EQ(zv_ui_paint_frame(&rows, &face, (zv_avatar_state_t)s, &chrome), ZV_OK);
        ZV_CHECK_EQ(zv_ui_paint_frame(&fills, &face, (zv_avatar_state_t)s, &chrome), ZV_OK);
        ZV_CHECK_EQ(memcmp(reference_rows, reference_fills, sizeof(reference_rows)), 0);
        if (s == (int)ZV_AVATAR_APPROVAL) {
            chrome.notice = "AN ACTION LABEL THAT EXCEEDS THE DISPLAY LIMIT";
            ZV_CHECK_EQ(zv_ui_paint_frame(&rows, &face, ZV_AVATAR_APPROVAL, &chrome), ZV_ERR_INVALID_ARG);
            chrome.notice = "DELETE ALL KEYS";
        }
        zv_ui_canvas_t failed = {.ctx = reference_rows, .fill_rect = framebuffer_fill, .draw_row = failed_row};
        ZV_CHECK_EQ(zv_ui_paint_frame(&failed, &face, (zv_avatar_state_t)s, &chrome), ZV_ERR_HARDWARE_GATE);
    }
}

static void test_round_control_touch(void)
{
    ZV_CHECK(zv_ui_touch_is_mic(126u, 329u));
    ZV_CHECK(zv_ui_touch_is_home(234u, 329u));
    ZV_CHECK(!zv_ui_touch_is_mic(14u, 305u));
    ZV_CHECK(!zv_ui_touch_is_home(300u, 350u));
    ZV_CHECK(!zv_ui_touch_is_home(UINT16_MAX, UINT16_MAX));
    int mic_hits = 0, home_hits = 0;
    for (uint16_t y = 0u; y < ZV_UI_H; y++) {
        for (uint16_t x = 0u; x < ZV_UI_W; x++) {
            bool mic = zv_ui_touch_is_mic(x, y), home = zv_ui_touch_is_home(x, y);
            if (mic || home) {
                int32_t dx = (int32_t)x - 180, dy = (int32_t)y - 180;
                ZV_CHECK(dx * dx + dy * dy < 180 * 180);
                ZV_CHECK(!(mic && home));
            }
            if (mic) mic_hits++;
            if (home) home_hits++;
        }
    }
    ZV_CHECK(mic_hits > 1000);
    ZV_CHECK(home_hits > 1000);
}

/*
 * Shell hit geometry. These assertions exist so the painted pixels and the
 * tappable pixels cannot silently drift apart: every constant here is the same
 * one the painter draws from.
 */
static void test_shell_hit_geometry(void)
{
    uint8_t index = 0xFFu;

    /* Home corner, unchanged from the original acceptance hit rect. */
    ZV_CHECK(zv_ui_home_hit(ZV_UI_HOME_X0, ZV_UI_HOME_Y0));
    ZV_CHECK(zv_ui_home_hit(ZV_UI_HOME_X1, ZV_UI_HOME_Y1));
    ZV_CHECK(!zv_ui_home_hit(100, 100));

    /* Operator-reported misses are now within disjoint shell finger targets. */
    ZV_CHECK(zv_ui_home_hit(264, 358));
    ZV_CHECK(zv_ui_home_hit(236, 358));
    ZV_CHECK(zv_ui_home_hit(235, 352));
    ZV_CHECK(zv_ui_volume_hit(122, 359));
    ZV_CHECK(zv_ui_volume_hit(98, 353));
    ZV_CHECK(!zv_ui_home_hit(236, 360));
    ZV_CHECK(!zv_ui_volume_hit(122, 360));
    for (int16_t x = 0; x < ZV_UI_W; x++) {
        ZV_CHECK(!(zv_ui_home_hit(x, 350) && zv_ui_volume_hit(x, 350)));
    }

    /* Menu rows are ZV_UI_MENU_ITEM_H tall and indexed from the top. */
    ZV_CHECK(zv_ui_menu_hit(ZV_UI_MENU_X0, ZV_UI_MENU_Y0, 4u, &index));
    ZV_CHECK_EQ(index, 0);
    ZV_CHECK(zv_ui_menu_hit(ZV_UI_MENU_X1, (int16_t)(ZV_UI_MENU_Y0 + ZV_UI_MENU_ITEM_H + 2), 4u, &index));
    ZV_CHECK_EQ(index, 1);
    ZV_CHECK(zv_ui_menu_hit(ZV_UI_MENU_X0, (int16_t)(ZV_UI_MENU_Y0 + 3 * ZV_UI_MENU_ITEM_H + 1), 4u, &index));
    ZV_CHECK_EQ(index, 3);

    /* Outside the rect horizontally, or in the tail past the last row. */
    ZV_CHECK(!zv_ui_menu_hit((int16_t)(ZV_UI_MENU_X0 - 1), ZV_UI_MENU_Y0, 4u, &index));
    ZV_CHECK(!zv_ui_menu_hit((int16_t)(ZV_UI_MENU_X1 + 1), ZV_UI_MENU_Y0, 4u, &index));
    ZV_CHECK(!zv_ui_menu_hit(ZV_UI_MENU_X0, (int16_t)(ZV_UI_MENU_Y0 + 4 * ZV_UI_MENU_ITEM_H), 4u, &index));
    ZV_CHECK(!zv_ui_menu_hit(ZV_UI_MENU_X0, (int16_t)(ZV_UI_MENU_Y0 - 1), 4u, &index));

    /* A caller exposing fewer rows must not be handed an index it never drew. */
    ZV_CHECK(!zv_ui_menu_hit(ZV_UI_MENU_X0, (int16_t)(ZV_UI_MENU_Y0 + 3 * ZV_UI_MENU_ITEM_H), 2u, &index));
    ZV_CHECK(!zv_ui_menu_hit(ZV_UI_MENU_X0, ZV_UI_MENU_Y0, 0u, &index));
    ZV_CHECK(!zv_ui_menu_hit(ZV_UI_MENU_X0, ZV_UI_MENU_Y0, 4u, NULL));

    /* The volume plate and the restart buttons never overlap each other. */
    ZV_CHECK(zv_ui_volume_hit(ZV_UI_VOLUME_X0, ZV_UI_VOLUME_Y0));
    ZV_CHECK(zv_ui_volume_hit(ZV_UI_VOLUME_X1, ZV_UI_VOLUME_Y1));
    ZV_CHECK(!zv_ui_volume_hit(ZV_UI_SHELL_VOLUME_TOUCH_X1, ZV_UI_VOLUME_Y0));

    ZV_CHECK(zv_ui_confirm_hit_yes(ZV_UI_CONFIRM_YES_X0, ZV_UI_CONFIRM_YES_Y0));
    ZV_CHECK(zv_ui_confirm_hit_yes(ZV_UI_CONFIRM_YES_X1, ZV_UI_CONFIRM_YES_Y1));
    ZV_CHECK(!zv_ui_confirm_hit_no(ZV_UI_CONFIRM_YES_X1, ZV_UI_CONFIRM_YES_Y1));
    ZV_CHECK(zv_ui_confirm_hit_no(ZV_UI_CONFIRM_NO_X0, ZV_UI_CONFIRM_NO_Y0));
    ZV_CHECK(zv_ui_confirm_hit_no(ZV_UI_CONFIRM_NO_X1, ZV_UI_CONFIRM_NO_Y1));
    ZV_CHECK(!zv_ui_confirm_hit_yes(ZV_UI_CONFIRM_NO_X0, ZV_UI_CONFIRM_NO_Y0));
    /* The gap between the buttons is a dead zone, not a silent YES. */
    ZV_CHECK(!zv_ui_confirm_hit_yes((int16_t)(ZV_UI_CONFIRM_YES_X1 + 5), ZV_UI_CONFIRM_YES_Y0));
    ZV_CHECK(!zv_ui_confirm_hit_no((int16_t)(ZV_UI_CONFIRM_YES_X1 + 5), ZV_UI_CONFIRM_NO_Y0));

    ZV_CHECK(zv_ui_volume_bar_hit(ZV_UI_VOLUME_BAR_X0, ZV_UI_VOLUME_BAR_Y0));
    ZV_CHECK(zv_ui_volume_bar_hit(ZV_UI_VOLUME_BAR_X1, ZV_UI_VOLUME_BAR_Y1));
    ZV_CHECK(!zv_ui_volume_bar_hit(ZV_UI_VOLUME_BAR_X0, (int16_t)(ZV_UI_VOLUME_BAR_Y1 + 1)));
}

/** A slider drag must produce a monotonic 0..100 and must saturate, not wrap. */
static void test_volume_from_x(void)
{
    ZV_CHECK_EQ(zv_ui_volume_from_x(ZV_UI_VOLUME_BAR_X0), 0);
    ZV_CHECK_EQ(zv_ui_volume_from_x((int16_t)(ZV_UI_VOLUME_BAR_X0 - 40)), 0);
    ZV_CHECK_EQ(zv_ui_volume_from_x(ZV_UI_VOLUME_BAR_X1), ZV_VOLUME_MAX);
    ZV_CHECK_EQ(zv_ui_volume_from_x((int16_t)(ZV_UI_VOLUME_BAR_X1 + 40)), ZV_VOLUME_MAX);

    const int16_t mid = (int16_t)((ZV_UI_VOLUME_BAR_X0 + ZV_UI_VOLUME_BAR_X1) / 2);
    uint8_t at_mid = zv_ui_volume_from_x(mid);
    ZV_CHECK(at_mid >= 48u && at_mid <= 52u);

    /* Monotonic across the whole travel. */
    uint8_t previous = 0u;
    for (int16_t x = ZV_UI_VOLUME_BAR_X0; x <= ZV_UI_VOLUME_BAR_X1; x += 4) {
        uint8_t now = zv_ui_volume_from_x(x);
        ZV_CHECK(now >= previous);
        previous = now;
    }
    ZV_CHECK_EQ(previous, ZV_VOLUME_MAX);
}

static void test_screen_names(void)
{
    ZV_CHECK_STR(zv_ui_screen_name(ZV_UI_SCREEN_HOME), "home");
    ZV_CHECK_STR(zv_ui_screen_name(ZV_UI_SCREEN_MENU), "menu");
    ZV_CHECK_STR(zv_ui_screen_name(ZV_UI_SCREEN_VOLUME), "volume");
    ZV_CHECK_STR(zv_ui_screen_name(ZV_UI_SCREEN_CONFIRM), "confirm");
}

/*
 * The new screens must paint through the same bounds-checked canvas as every
 * other frame; a chrome value of 100% volume has to stay on the panel.
 */
/*
 * Regression guard for an accepted screen.
 *
 * The eight-state menu overlay is already accepted on hardware. Every existing
 * caller builds its chrome with memset, which leaves menu_focus at 0 -- and 0 is
 * the first row's index. A focus band keyed on menu_focus alone therefore paints
 * on a screen nobody asked to change, so this asserts the drawn pixel sequence is
 * byte-for-byte identical whether the caller says "focus 0" or "no focus".
 */
typedef struct {
    uint32_t hash;
    size_t count;
} seq_t;

static zv_status_t seq_fill(void *ctx, int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color)
{
    seq_t *s = (seq_t *)ctx;
    const uint32_t vals[5] = {(uint32_t)(int32_t)x0, (uint32_t)(int32_t)y0, (uint32_t)(int32_t)x1,
                              (uint32_t)(int32_t)y1, (uint32_t)color};
    uint32_t h = s->hash;
    for (int i = 0; i < 5; i++) {
        h ^= vals[i];
        h *= 16777619u;
    }
    s->hash = h;
    s->count++;
    return ZV_OK;
}

/** Renders the accepted menu overlay and hashes every fill it emits. */
static void menu_overlay_hash(uint8_t menu_focus, zv_ui_screen_t screen, uint32_t *out_hash, size_t *out_count)
{
    zv_avatar_model_t model;
    seq_t rec = {2166136261u, 0u};
    zv_ui_canvas_t canvas = {.ctx = &rec, .fill_rect = seq_fill};
    zv_ui_chrome_t chrome;
    zv_face_frame_t face;

    ZV_CHECK_EQ(zv_avatar_init(&model, 0u, 0x4c494e4bu), ZV_OK);
    memset(&chrome, 0, sizeof(chrome));
    chrome.use_zenti_head = true;
    chrome.menu_open = true;
    chrome.menu_count = 4u;
    chrome.menu_items[0] = "NETWORK";
    chrome.menu_items[1] = "VOLUME";
    chrome.menu_items[2] = "MUTE";
    chrome.menu_items[3] = "RESTART";
    chrome.menu_focus = menu_focus;
    chrome.screen = screen;
    ZV_CHECK_EQ(zv_avatar_frame(&model, 10u, NULL, 0u, &face), ZV_OK);
    ZV_CHECK_EQ(zv_ui_paint_frame(&canvas, &face, ZV_AVATAR_IDLE, &chrome), ZV_OK);
    *out_hash = rec.hash;
    *out_count = rec.count;
}

static void test_menu_focus_never_alters_the_accepted_screen(void)
{
    uint32_t zeroed_hash, explicit_hash, shell_hash;
    size_t zeroed_count, explicit_count, shell_count;

    /* A memset chrome (menu_focus == 0) must look exactly like one that
       explicitly asked for no focus. */
    menu_overlay_hash(0u, ZV_UI_SCREEN_HOME, &zeroed_hash, &zeroed_count);
    menu_overlay_hash(ZV_UI_NO_FOCUS, ZV_UI_SCREEN_HOME, &explicit_hash, &explicit_count);
    ZV_CHECK_EQ(zeroed_hash, explicit_hash);
    ZV_CHECK_EQ(zeroed_count, explicit_count);
    ZV_CHECK(zeroed_count > 0u);

    /* The shell's menu screen is where the focus band is allowed to appear. */
    menu_overlay_hash(1u, ZV_UI_SCREEN_MENU, &shell_hash, &shell_count);
    ZV_CHECK(shell_hash != explicit_hash);
}

static void test_new_screens_stay_in_bounds(void)
{
    zv_avatar_model_t model;
    ZV_CHECK_EQ(zv_avatar_init(&model, 0u, 0x4c494e4bu), ZV_OK);
    rec_t rec;
    memset(&rec, 0, sizeof(rec));
    zv_ui_canvas_t canvas = {.ctx = &rec, .fill_rect = rec_fill, .draw_row = rec_row};

    static const zv_ui_screen_t screens[] = {ZV_UI_SCREEN_HOME, ZV_UI_SCREEN_MENU, ZV_UI_SCREEN_VOLUME,
                                             ZV_UI_SCREEN_CONFIRM};
    for (size_t i = 0; i < sizeof(screens) / sizeof(screens[0]); i++) {
        zv_face_frame_t face;
        ZV_CHECK_EQ(zv_avatar_frame(&model, 10u, NULL, 0u, &face), ZV_OK);
        zv_ui_chrome_t chrome;
        memset(&chrome, 0, sizeof(chrome));
        chrome.use_zenti_head = true;
        chrome.show_home = true;
        chrome.show_volume = true;
        chrome.clock_text = "09:05";
        chrome.screen = screens[i];
        chrome.menu_open = screens[i] == ZV_UI_SCREEN_MENU;
        chrome.menu_count = chrome.menu_open ? 4u : 0u;
        chrome.menu_items[0] = "NETWORK";
        chrome.menu_items[1] = "VOLUME";
        chrome.menu_items[2] = "MUTE";
        chrome.menu_items[3] = "RESTART";
        chrome.menu_focus = chrome.menu_open ? 2u : (uint8_t)ZV_UI_NO_FOCUS;
        chrome.status_text = "SHELL";
        chrome.detail_text = "TAP A ROW";
        zv_volume_t vol;
        /* Worst case for the bar: full scale. */
        zv_volume_init(&vol, ZV_VOLUME_MAX);
        chrome.volume = vol;
        memset(&rec, 0, sizeof(rec));
        ZV_CHECK_EQ(zv_ui_paint_frame(&canvas, &face, ZV_AVATAR_IDLE, &chrome), ZV_OK);
        ZV_CHECK(!rec.overflow);
        ZV_CHECK(rec.count > 0u);
    }
}

/* Compare the final slider label pixels with the requested volume, including
 * full scale. Background/head paints are overwritten by the volume overlay. */
typedef struct { uint16_t pixels[21][80]; } label_pixels_t;
static zv_status_t label_fill(void *ctx, int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color)
{
    label_pixels_t *label = ctx;
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            if (x >= 140 && x < 220 && y >= 186 && y < 207)
                label->pixels[y - 186][x - 140] = color;
        }
    }
    return ZV_OK;
}
static void test_volume_label_matches_level(void)
{
    static const uint8_t levels[] = {0u, 9u, 40u, 100u};
    static const char *labels[] = {"0%", "9%", "40%", "100%"};
    zv_avatar_model_t model;
    zv_face_frame_t face;
    ZV_CHECK_EQ(zv_avatar_init(&model, 0u, 42u), ZV_OK);
    ZV_CHECK_EQ(zv_avatar_frame(&model, 10u, NULL, 0u, &face), ZV_OK);
    for (size_t i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
        label_pixels_t actual = {0}, expected = {0};
        zv_ui_canvas_t canvas = {.ctx = &actual, .fill_rect = label_fill};
        zv_ui_canvas_t text = {.ctx = &expected, .fill_rect = label_fill};
        zv_ui_chrome_t chrome = {0};
        chrome.screen = ZV_UI_SCREEN_VOLUME;
        zv_volume_init(&chrome.volume, levels[i]);
        ZV_CHECK_EQ(zv_ui_paint_frame(&canvas, &face, ZV_AVATAR_IDLE, &chrome), ZV_OK);
        ZV_CHECK_EQ(label_fill(&expected, 140, 186, 220, 207, zv_rgb888_to_rgb565(ZV_COLOR_NAVY)), ZV_OK);
        int16_t x = (int16_t)((ZV_UI_W - zv_ui_text_width(labels[i], 3u)) / 2);
        ZV_CHECK_EQ(zv_ui_paint_text(&text, x, 186, labels[i], zv_rgb888_to_rgb565(0xD9F2FFu), 3u), ZV_OK);
        ZV_CHECK(memcmp(&actual, &expected, sizeof(actual)) == 0);
    }
}

typedef struct { uint16_t pixels[14][80]; } accept_pixels_t;
static zv_status_t accept_fill(void *ctx, int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color)
{
    accept_pixels_t *label = ctx;
    for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) {
        if (x >= 140 && x < 220 && y >= 278 && y < 292)
            label->pixels[y - 278][x - 140] = color;
    }
    return ZV_OK;
}
static void test_volume_accept_is_visible_and_separate_from_slider(void)
{
    zv_avatar_model_t model;
    zv_face_frame_t face;
    zv_ui_chrome_t chrome = {0};
    accept_pixels_t actual = {0}, expected = {0};
    zv_ui_canvas_t canvas = {.ctx = &actual, .fill_rect = accept_fill};
    zv_ui_canvas_t text = {.ctx = &expected, .fill_rect = accept_fill};
    ZV_CHECK_EQ(zv_avatar_init(&model, 0u, 42u), ZV_OK);
    ZV_CHECK_EQ(zv_avatar_frame(&model, 10u, NULL, 0u, &face), ZV_OK);
    chrome.screen = ZV_UI_SCREEN_VOLUME;
    zv_volume_init(&chrome.volume, 70u);
    ZV_CHECK_EQ(zv_ui_paint_frame(&canvas, &face, ZV_AVATAR_IDLE, &chrome), ZV_OK);
    ZV_CHECK_EQ(accept_fill(&expected, 140, 278, 220, 292,
                          zv_rgb888_to_rgb565_scaled(face.ring_rgb, 250u)), ZV_OK);
    ZV_CHECK_EQ(zv_ui_paint_text(&text, (int16_t)((ZV_UI_W - zv_ui_text_width("ACCEPT", 2u)) / 2),
                               278, "ACCEPT", zv_rgb888_to_rgb565(0xD9F2FFu), 2u), ZV_OK);
    ZV_CHECK(memcmp(&actual, &expected, sizeof(actual)) == 0);
    ZV_CHECK(zv_ui_volume_accept_hit(180, 285));
    ZV_CHECK(!zv_ui_volume_bar_hit(180, 285));
    ZV_CHECK(!zv_ui_volume_accept_hit(180, 254));
    ZV_CHECK(!zv_ui_volume_accept_hit(ZV_UI_VOLUME_ACCEPT_X0 - 1, 285));
    ZV_CHECK_EQ(chrome.volume.level, 70u);
}

typedef struct {
    uint16_t status[14][240], title[14][84], back[14][80], header[50][360];
    uint16_t wifi[28][44], lock[23][16], battery[34][36], clock[28][120];
} network_pixels_t;
static zv_status_t network_fill(void *ctx, int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color)
{
    network_pixels_t *pixels = ctx;
    if (x0 < 0 || y0 < 0 || x1 > 360 || y1 > 360) return ZV_ERR_INVALID_STATE;
    for (int y=y0; y<y1; ++y) for (int x=x0; x<x1; ++x) {
        if (y<50) pixels->header[y][x]=color;
        if (x>=60 && x<300 && y>=184 && y<198) pixels->status[y-184][x-60]=color;
        if (x>=138 && x<222 && y>=140 && y<154) pixels->title[y-140][x-138]=color;
        if (x>=140 && x<220 && y>=278 && y<292) pixels->back[y-278][x-140]=color;
        if (x>=52 && x<96 && y>=56 && y<84) pixels->wifi[y-56][x-52]=color;
        if (x>=258 && x<274 && y>=58 && y<81) pixels->lock[y-58][x-258]=color;
        if (x>=278 && x<314 && y>=58 && y<92) pixels->battery[y-58][x-278]=color;
        if (x>=120 && x<240 && y>=47 && y<75) pixels->clock[y-47][x-120]=color;
    }
    return ZV_OK;
}
static void test_network_status_pixels_and_back(void)
{
    static const char *labels[] = {"AUTHENTICATED ONLINE", "GATEWAY OFFLINE", "NOT CHECKED"};
    zv_avatar_model_t model;
    zv_face_frame_t face;
    ZV_CHECK_EQ(zv_avatar_init(&model,0u,42u),ZV_OK);
    ZV_CHECK_EQ(zv_avatar_frame(&model,10u,NULL,0u,&face),ZV_OK);
    network_pixels_t previous = {0};
    for (size_t i=0; i<sizeof(labels)/sizeof(labels[0]); ++i) {
        network_pixels_t actual = {0}, expected = {0};
        zv_ui_canvas_t canvas = {.ctx=&actual,.fill_rect=network_fill};
        zv_ui_canvas_t text = {.ctx=&expected,.fill_rect=network_fill};
        zv_ui_chrome_t chrome = {0};
        chrome.screen=ZV_UI_SCREEN_NETWORK;
        chrome.detail_text=labels[i];chrome.secure_link=(i==0);chrome.gateway_online=(i==0);
        chrome.show_connectivity=true;chrome.signal_bars=(i==0)?3u:0u;
        chrome.battery_ok=true;chrome.battery_percent=99u;
        chrome.date_text="THU, OCT 8";chrome.clock_text="11:20";
        ZV_CHECK_EQ(zv_ui_paint_frame(&canvas,&face,ZV_AVATAR_IDLE,&chrome),ZV_OK);
        ZV_CHECK_EQ(network_fill(&expected,40,120,320,270,zv_rgb888_to_rgb565(ZV_COLOR_NAVY)),ZV_OK);
        ZV_CHECK_EQ(zv_ui_paint_text(&text,138,140,"NETWORK",zv_rgb888_to_rgb565(0xD9F2FFu),2u),ZV_OK);
        int16_t x=(int16_t)((360-zv_ui_text_width(labels[i],2u))/2);
        ZV_CHECK_EQ(zv_ui_paint_text(&text,x,184,labels[i],zv_rgb888_to_rgb565(0xD9F2FFu),2u),ZV_OK);
        ZV_CHECK_EQ(network_fill(&expected,140,278,220,292,zv_rgb888_to_rgb565_scaled(face.ring_rgb,250u)),ZV_OK);
        ZV_CHECK_EQ(zv_ui_paint_text(&text,156,278,"BACK",zv_rgb888_to_rgb565(0xD9F2FFu),2u),ZV_OK);
        ZV_CHECK(memcmp(actual.status,expected.status,sizeof(actual.status))==0);
        ZV_CHECK(memcmp(actual.title,expected.title,sizeof(actual.title))==0);
        ZV_CHECK(memcmp(actual.back,expected.back,sizeof(actual.back))==0);
        if (i>0) {
            ZV_CHECK(memcmp(actual.header,previous.header,sizeof(actual.header))==0);
            ZV_CHECK(memcmp(actual.clock,previous.clock,sizeof(actual.clock))==0);
            ZV_CHECK(memcmp(actual.battery,previous.battery,sizeof(actual.battery))==0);
        }
        if (i==1) {
            ZV_CHECK(memcmp(actual.wifi,previous.wifi,sizeof(actual.wifi))!=0);
            ZV_CHECK(memcmp(actual.lock,previous.lock,sizeof(actual.lock))!=0);
        }
        ZV_CHECK_EQ(actual.battery[9][5],zv_rgb888_to_rgb565(0x24F6A4u));
        previous=actual;
    }
    ZV_CHECK_EQ(strcmp(zv_ui_screen_name(ZV_UI_SCREEN_NETWORK),"network"),0);
    ZV_CHECK(zv_ui_volume_accept_hit(180,285));
}

static void test_nous_eight_states_and_safe_selection(void)
{
    zv_ui_select_avatar("nous-girl");
    ZV_CHECK(zv_ui_nous_selected());
    zv_avatar_model_t model;
    ZV_CHECK_EQ(zv_avatar_init(&model, 0u, 42u), ZV_OK);
    for (int state = 0; state < ZV_AVATAR_STATE_COUNT; ++state) {
        rec_t rec = {0};
        zv_ui_canvas_t canvas = {.ctx = &rec, .fill_rect = rec_fill, .draw_row = rec_row};
        zv_ui_chrome_t chrome = {0}; chrome.use_zenti_head = true;
        zv_face_frame_t face;
        ZV_CHECK_EQ(zv_avatar_set_state(&model, (zv_avatar_state_t)state, 0u), ZV_OK);
        ZV_CHECK_EQ(zv_avatar_frame(&model, 100u, NULL, 0u, &face), ZV_OK);
        ZV_CHECK_EQ(zv_ui_paint_frame(&canvas, &face, (zv_avatar_state_t)state, &chrome), ZV_OK);
        ZV_CHECK(!rec.overflow);
        ZV_CHECK(rec.count >= 224u);
    }
    zv_ui_select_avatar("unknown"); ZV_CHECK(!zv_ui_nous_selected());
    zv_ui_select_avatar(NULL); ZV_CHECK(!zv_ui_nous_selected());
}

void test_suite_ui_draw(void)
{
    ZV_CHECK(zv_ui_volume_mute_hit(180, 226));
    ZV_CHECK(!zv_ui_volume_bar_hit(180, 226));
    ZV_CHECK(!zv_ui_volume_accept_hit(180, 226));
    ZV_CHECK(!zv_ui_volume_mute_hit(180, 254));
    ZV_CHECK(!zv_ui_volume_mute_hit(180, 285));
    for (int16_t y=ZV_UI_VOLUME_MUTE_Y0;y<=ZV_UI_VOLUME_MUTE_Y1;y++) {
        for (int16_t x=ZV_UI_VOLUME_MUTE_X0;x<=ZV_UI_VOLUME_MUTE_X1;x++) {
            ZV_CHECK(zv_ui_volume_mute_hit(x,y));
            ZV_CHECK(!zv_ui_volume_bar_hit(x,y) && !zv_ui_volume_accept_hit(x,y));
        }
    }
    ZV_CHECK(zv_ui_touch_is_voice_function(180, 180));
    ZV_CHECK(zv_ui_touch_is_voice_function(100, 180));
    ZV_CHECK(zv_ui_touch_is_voice_function(260, 180));
    ZV_CHECK(zv_ui_touch_is_voice_function(180, 288));
    ZV_CHECK(!zv_ui_touch_is_voice_function(180, 289));
    ZV_CHECK(!zv_ui_touch_is_voice_function(126, 329));
    ZV_CHECK(zv_ui_touch_is_volume(126,329));
    ZV_CHECK(zv_ui_cancel_hit(180,329));
    ZV_CHECK(zv_ui_cancel_hit(180,355)); /* label is tappable too */
    unsigned overlap=0, outside=0;
    for (uint16_t y=0;y<360;++y) for (uint16_t x=0;x<360;++x) {
        unsigned hits=(unsigned)zv_ui_touch_is_volume(x,y)+(unsigned)zv_ui_cancel_hit(x,y)+
                      (unsigned)zv_ui_touch_is_home(x,y)+(unsigned)zv_ui_touch_is_voice_function(x,y);
        if (hits>1u) ++overlap;
        int32_t dx=(int32_t)x-180,dy=(int32_t)y-180;
        if (hits && dx*dx+dy*dy>=180*180) ++outside;
    }
    ZV_CHECK_EQ(overlap,0u); ZV_CHECK_EQ(outside,0u);
    ZV_CHECK(!zv_ui_touch_is_voice_function(234, 329));
    ZV_CHECK(!zv_ui_touch_is_voice_function(180, 40));
    ZV_CHECK(!zv_ui_touch_is_voice_function(0, 180));
    test_nous_eight_states_and_safe_selection();
    test_rgb565();
    test_text();
    test_all_eight_states();
    test_ring_colors();
    test_menu_overlay();
    test_v2_interface_chrome();
    test_speaking_mouth_follows_level();
    test_zenti_head_sprite();
    test_shell_hit_geometry();
    test_volume_from_x();
    test_screen_names();
    test_menu_focus_never_alters_the_accepted_screen();
    test_new_screens_stay_in_bounds();
    test_volume_label_matches_level();
    test_volume_accept_is_visible_and_separate_from_slider();
    test_network_status_pixels_and_back();
    test_low_battery_escalation();
    test_booting_screen();
    test_link_cues_accept_lowercase();
    test_zenti_state_effects_and_approval_panel();
    test_reference_transport_parity();
    test_mouth_changes_never_replace_the_surrounding_portrait();
    test_nous_opens_below_the_upper_lip_and_preserves_other_expressions();
    test_home_volume_percent_and_dedicated_cancel_pixels();
    test_round_control_touch();
}
