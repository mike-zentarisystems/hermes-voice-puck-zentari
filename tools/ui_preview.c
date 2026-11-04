/* Actual portable firmware painter rendered into a host-only RGB565 buffer. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zv_ui_draw.h"
static uint16_t pixels[ZV_UI_W * ZV_UI_H];
static size_t fills;
static zv_status_t record_fill(void *ctx, int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color)
{
    (void)ctx;
    if (x0 < 0 || y0 < 0 || x1 > ZV_UI_W || y1 > ZV_UI_H || x1 <= x0 || y1 <= y0) return ZV_ERR_INVALID_ARG;
    fills++;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) pixels[y * ZV_UI_W + x] = color;
    return ZV_OK;
}
static zv_status_t record_row(void *ctx, int16_t x, int16_t y, uint16_t width, const uint16_t *row)
{
    (void)ctx;
    if (row == NULL || x < 0 || y < 0 || y >= ZV_UI_H || x + width > ZV_UI_W) return ZV_ERR_INVALID_ARG;
    fills++;
    memcpy(&pixels[y * ZV_UI_W + x], row, (size_t)width * sizeof(uint16_t));
    return ZV_OK;
}
int main(int argc, char **argv)
{
    if (argc != 3 && argc != 4) { fprintf(stderr, "usage: zv_ui_preview STATE OUT.ppm [CLOCK]\n"); return 2; }
    if (getenv("ZV_PREVIEW_AVATAR") != NULL) zv_ui_select_avatar(getenv("ZV_PREVIEW_AVATAR"));
    zv_avatar_state_t state = ZV_AVATAR_IDLE;
    const char *status = argv[1];
    bool menu = strcmp(status, "menu") == 0;
    for (int i = 0; i < (int)ZV_AVATAR_STATE_COUNT; i++)
        if (strcmp(status, zv_avatar_state_name((zv_avatar_state_t)i)) == 0) state = (zv_avatar_state_t)i;
    if (strcmp(status, "connecting") == 0) state = ZV_AVATAR_THINKING;
    if (strcmp(status, "offline") == 0) state = ZV_AVATAR_ERROR;
    zv_avatar_model_t model;
    zv_face_frame_t face;
    zv_mouth_shape_t mouth = {.open = 700u, .width = 500u, .round = 100u};
    if (getenv("ZV_PREVIEW_MOUTH") != NULL) {
        long opening = strtol(getenv("ZV_PREVIEW_MOUTH"), NULL, 10);
        mouth.open = (uint16_t)(opening < 0 ? 0 : opening > 1000 ? 1000 : opening);
    }
    if (zv_avatar_init(&model, 0u, 42u) != ZV_OK || zv_avatar_set_state(&model, state, 0u) != ZV_OK ||
        zv_avatar_frame(&model, 100u, state == ZV_AVATAR_SPEAKING ? &mouth : NULL, 700u, &face) != ZV_OK) return 3;
    zv_ui_chrome_t chrome;
    memset(&chrome, 0, sizeof(chrome));
    chrome.use_zenti_head = true; chrome.show_connectivity = true; chrome.show_home = true;
    chrome.show_volume = true; zv_volume_init(&chrome.volume,40u);
    chrome.show_cancel = state == ZV_AVATAR_LISTENING || state == ZV_AVATAR_THINKING || state == ZV_AVATAR_SPEAKING;
    chrome.battery_ok = true; chrome.battery_percent = 85u;
    chrome.date_text = "TUE, OCT 6"; chrome.clock_text = argc == 4 ? argv[3] : "9:32 AM"; chrome.status_text = status;
    chrome.signal_bars = 3u;
    chrome.gateway_online = strcmp(status, "offline") != 0 && strcmp(status, "connecting") != 0 && strcmp(status, "starting") != 0;
    chrome.secure_link = chrome.gateway_online;
    if (state == ZV_AVATAR_APPROVAL) chrome.notice = "DELETE ALL KEYS";
    if (strcmp(status, "offline") == 0) chrome.notice = "LOCAL ONLY - RETRY 1234567890 MS";
    if (strcmp(status, "connecting") == 0) chrome.notice = "ACTIONS LOCKED";
    if (state == ZV_AVATAR_SPEAKING || state == ZV_AVATAR_LISTENING) {
        chrome.show_wave = true; chrome.wave_len = ZV_UI_WAVE_BARS;
        for (int i = 0; i < ZV_UI_WAVE_BARS; i++) chrome.wave[i] = (int16_t)((i % 5) * 350 - 700);
    }
    chrome.menu_open = menu; chrome.menu_count = 5u;
    chrome.menu_items[0] = "MUTE"; chrome.menu_items[1] = "BRIGHTNESS"; chrome.menu_items[2] = "WIFI + LINK";
    chrome.menu_items[3] = "DEVICE INFO"; chrome.menu_items[4] = "RESTART";
    if (strcmp(status, "volume") == 0 || strcmp(status, "volume-muted") == 0) {
        chrome.screen = ZV_UI_SCREEN_VOLUME;
        chrome.status_text = "VOLUME";
        (void)zv_volume_set_muted(&chrome.volume, strcmp(status, "volume-muted") == 0);
    }
    zv_ui_canvas_t canvas = {.ctx = NULL, .fill_rect = record_fill, .draw_row = record_row};
    if (zv_ui_paint_frame(&canvas, &face, state, &chrome) != ZV_OK) return 4;
    FILE *file = NULL;
#ifdef _MSC_VER
    if (fopen_s(&file, argv[2], "wb") != 0) return 5;
#else
    file = fopen(argv[2], "wb"); if (file == NULL) return 5;
#endif
    fprintf(file, "P6\n%d %d\n255\n", ZV_UI_W, ZV_UI_H);
    for (int y = 0; y < ZV_UI_H; y++) {
        for (int x = 0; x < ZV_UI_W; x++) {
            uint16_t pixel = pixels[y * ZV_UI_W + x]; int dx = x - 180, dy = y - 180;
            if (dx * dx + dy * dy >= 180 * 180) pixel = 0u;
            unsigned char rgb[3] = {(unsigned char)(((pixel >> 11) & 31u) * 255u / 31u),
                                    (unsigned char)(((pixel >> 5) & 63u) * 255u / 63u),
                                    (unsigned char)((pixel & 31u) * 255u / 31u)};
            if (fwrite(rgb, 1u, 3u, file) != 3u) { fclose(file); return 6; }
        }
    }
    fclose(file); printf("%s fills=%zu\n", status, fills); return 0;
}
