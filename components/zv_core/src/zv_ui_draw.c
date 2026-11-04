/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 */
#include "zv_ui_draw.h"

#include <string.h>

#include "zv_zenti_sprite.h"
#include "zv_nous_sprite.inc"
static bool s_nous;
void zv_ui_select_avatar(const char *id) { s_nous = id != NULL && strcmp(id, "nous-girl") == 0; }
bool zv_ui_nous_selected(void) { return s_nous; }

/* Quarter-wave cosine, permille, degrees 0..90 inclusive. */
static const int16_t k_cos_permille[91] = {
    1000, 999, 999, 998, 998, 996, 995, 993, 990, 988, 985, 982, 978, 974, 970, 966, 961, 956, 951, 946,
    940,  934, 927, 921, 914, 906, 899, 891, 883, 875, 866, 857, 848, 839, 829, 819, 809, 799, 788, 777,
    766,  755, 743, 731, 719, 707, 695, 682, 669, 656, 643, 629, 616, 602, 588, 574, 559, 545, 530, 515,
    500,  485, 469, 454, 438, 423, 407, 391, 375, 358, 342, 326, 309, 292, 276, 259, 242, 225, 208, 191,
    174,  156, 139, 122, 105, 87,  70,  52,  35,  17,  0,
};

/*
 * 5x7 column-major glyphs, bit 0 = top row. Subset covers the status,
 * menu, and notice strings the acceptance demo and wire names need.
 */
static const uint8_t k_font[96][5] = {
    [' ' - 32] = {0x00, 0x00, 0x00, 0x00, 0x00},
    ['%' - 32] = {0x23, 0x13, 0x08, 0x64, 0x62},
    [',' - 32] = {0x00, 0x50, 0x30, 0x00, 0x00},
    ['+' - 32] = {0x08, 0x08, 0x3E, 0x08, 0x08},
    ['!' - 32] = {0x00, 0x00, 0x5F, 0x00, 0x00},
    ['-' - 32] = {0x08, 0x08, 0x08, 0x08, 0x08},
    ['.' - 32] = {0x00, 0x00, 0x60, 0x00, 0x00},
    ['/' - 32] = {0x20, 0x10, 0x08, 0x04, 0x02},
    ['0' - 32] = {0x3E, 0x51, 0x49, 0x45, 0x3E},
    ['1' - 32] = {0x00, 0x42, 0x7F, 0x40, 0x00},
    ['2' - 32] = {0x42, 0x61, 0x51, 0x49, 0x46},
    ['3' - 32] = {0x21, 0x41, 0x45, 0x4B, 0x31},
    ['4' - 32] = {0x18, 0x14, 0x12, 0x7F, 0x10},
    ['5' - 32] = {0x27, 0x45, 0x45, 0x45, 0x39},
    ['6' - 32] = {0x3C, 0x4A, 0x49, 0x49, 0x30},
    ['7' - 32] = {0x01, 0x71, 0x09, 0x05, 0x03},
    ['8' - 32] = {0x36, 0x49, 0x49, 0x49, 0x36},
    ['9' - 32] = {0x06, 0x49, 0x49, 0x29, 0x1E},
    [':' - 32] = {0x00, 0x36, 0x36, 0x00, 0x00},
    ['A' - 32] = {0x7E, 0x11, 0x11, 0x11, 0x7E},
    ['B' - 32] = {0x7F, 0x49, 0x49, 0x49, 0x36},
    ['C' - 32] = {0x3E, 0x41, 0x41, 0x41, 0x22},
    ['D' - 32] = {0x7F, 0x41, 0x41, 0x22, 0x1C},
    ['E' - 32] = {0x7F, 0x49, 0x49, 0x49, 0x41},
    ['F' - 32] = {0x7F, 0x09, 0x09, 0x09, 0x01},
    ['G' - 32] = {0x3E, 0x41, 0x49, 0x49, 0x3A},
    ['H' - 32] = {0x7F, 0x08, 0x08, 0x08, 0x7F},
    ['I' - 32] = {0x00, 0x41, 0x7F, 0x41, 0x00},
    ['J' - 32] = {0x20, 0x40, 0x41, 0x3F, 0x01},
    ['K' - 32] = {0x7F, 0x08, 0x14, 0x22, 0x41},
    ['L' - 32] = {0x7F, 0x40, 0x40, 0x40, 0x40},
    ['M' - 32] = {0x7F, 0x02, 0x0C, 0x02, 0x7F},
    ['N' - 32] = {0x7F, 0x04, 0x08, 0x10, 0x7F},
    ['O' - 32] = {0x3E, 0x41, 0x41, 0x41, 0x3E},
    ['P' - 32] = {0x7F, 0x09, 0x09, 0x09, 0x06},
    ['Q' - 32] = {0x3E, 0x41, 0x51, 0x21, 0x5E},
    ['R' - 32] = {0x7F, 0x09, 0x19, 0x29, 0x46},
    ['S' - 32] = {0x46, 0x49, 0x49, 0x49, 0x31},
    ['T' - 32] = {0x01, 0x01, 0x7F, 0x01, 0x01},
    ['U' - 32] = {0x3F, 0x40, 0x40, 0x40, 0x3F},
    ['V' - 32] = {0x1F, 0x20, 0x40, 0x20, 0x1F},
    ['W' - 32] = {0x3F, 0x40, 0x38, 0x40, 0x3F},
    ['X' - 32] = {0x63, 0x14, 0x08, 0x14, 0x63},
    ['Y' - 32] = {0x07, 0x08, 0x70, 0x08, 0x07},
    ['Z' - 32] = {0x61, 0x51, 0x49, 0x45, 0x43},
};

static int16_t cos_permille(int deg)
{
    deg %= 360;
    if (deg < 0) {
        deg += 360;
    }
    if (deg <= 90) {
        return k_cos_permille[deg];
    }
    if (deg <= 180) {
        return (int16_t)-k_cos_permille[180 - deg];
    }
    if (deg <= 270) {
        return (int16_t)-k_cos_permille[deg - 180];
    }
    return k_cos_permille[360 - deg];
}

static int16_t sin_permille(int deg)
{
    return cos_permille(deg - 90);
}

static zv_status_t fill(zv_ui_canvas_t *c, int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color)
{
    if (c == NULL || c->fill_rect == NULL) {
        return ZV_ERR_INVALID_ARG;
    }
    if (x1 <= x0 || y1 <= y0) {
        return ZV_OK;
    }
    if (x0 < 0) {
        x0 = 0;
    }
    if (y0 < 0) {
        y0 = 0;
    }
    if (x1 > ZV_UI_W) {
        x1 = (int16_t)ZV_UI_W;
    }
    if (y1 > ZV_UI_H) {
        y1 = (int16_t)ZV_UI_H;
    }
    if (x1 <= x0 || y1 <= y0) {
        return ZV_OK;
    }
    return c->fill_rect(c->ctx, x0, y0, x1, y1, color);
}

/* Horizontal inset of a circular corner at row `dy` from the corner edge. */
static int16_t corner_inset(int16_t dy, int16_t radius)
{
    if (radius <= 0 || dy < 0 || dy >= radius) {
        return 0;
    }
    int32_t ry = (int32_t)radius - dy;
    int32_t span = 0;
    for (int32_t x = radius; x >= 0; x--) {
        if (x * x + ry * ry <= (int32_t)radius * radius) {
            span = x;
            break;
        }
    }
    return (int16_t)(radius - span);
}

static zv_status_t paint_round_rect(zv_ui_canvas_t *c, int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                                    int16_t radius, uint16_t color)
{
    if (x1 <= x0 || y1 <= y0) return ZV_OK;
    int16_t w = (int16_t)(x1 - x0), h = (int16_t)(y1 - y0);
    if (radius * 2 > w) radius = (int16_t)(w / 2);
    if (radius * 2 > h) radius = (int16_t)(h / 2);
    if (radius < 0) radius = 0;
    int16_t previous = -1, first = 0;
    for (int16_t dy = 0; dy <= h; dy++) {
        int16_t inset = 0;
        if (dy < radius) inset = corner_inset(dy, radius);
        else if (dy >= h - radius && dy < h) inset = corner_inset((int16_t)(h - 1 - dy), radius);
        if (inset != previous || dy == h) {
            if (previous >= 0) {
                zv_status_t err = fill(c, (int16_t)(x0 + previous), (int16_t)(y0 + first),
                                      (int16_t)(x1 - previous), (int16_t)(y0 + dy), color);
                if (err != ZV_OK) return err;
            }
            previous = inset;
            first = dy;
        }
    }
    return ZV_OK;
}

uint16_t zv_rgb_to_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((uint16_t)(r & 0xF8u) << 8) | ((uint16_t)(g & 0xFCu) << 3) | ((uint16_t)b >> 3));
}

uint16_t zv_rgb888_to_rgb565(uint32_t rgb888)
{
    return zv_rgb_to_rgb565((uint8_t)((rgb888 >> 16) & 0xFFu), (uint8_t)((rgb888 >> 8) & 0xFFu),
                            (uint8_t)(rgb888 & 0xFFu));
}

uint16_t zv_rgb888_to_rgb565_scaled(uint32_t rgb888, uint16_t intensity_permille)
{
    if (intensity_permille > 1000u) {
        intensity_permille = 1000u;
    }
    uint32_t r = ((rgb888 >> 16) & 0xFFu) * intensity_permille / 1000u;
    uint32_t g = ((rgb888 >> 8) & 0xFFu) * intensity_permille / 1000u;
    uint32_t b = (rgb888 & 0xFFu) * intensity_permille / 1000u;
    return zv_rgb_to_rgb565((uint8_t)r, (uint8_t)g, (uint8_t)b);
}

static zv_status_t paint_ellipse(zv_ui_canvas_t *c, int16_t cx, int16_t cy, int16_t rx, int16_t ry, uint16_t color)
{
    if (rx <= 0 || ry <= 0) return ZV_OK;
    int16_t previous = -1, first = (int16_t)-ry;
    for (int16_t dy = (int16_t)-ry; dy <= ry; dy++) {
        int16_t span = 0;
        int32_t num = (int32_t)ry * ry - (int32_t)dy * dy;
        if (num > 0) {
            for (int32_t x = rx; x > 0; x--) {
                if (x * x * (int32_t)ry * ry <= num * (int32_t)rx * rx) {
                    span = (int16_t)x;
                    break;
                }
            }
        }
        if (span != previous || dy == ry) {
            if (previous > 0) {
                zv_status_t err = fill(c, (int16_t)(cx - previous), (int16_t)(cy + first),
                                      (int16_t)(cx + previous), (int16_t)(cy + dy), color);
                if (err != ZV_OK) return err;
            }
            previous = span;
            first = dy;
        }
    }
    return ZV_OK;
}

static zv_status_t paint_ring(zv_ui_canvas_t *c, int16_t cx, int16_t cy, int16_t r_mid, int16_t thickness,
                              uint32_t rgb888, uint16_t intensity_permille, bool sweep, uint16_t sweep_deg)
{
    const int segs = 72;
    const int span_deg = 18;
    uint16_t color = zv_rgb888_to_rgb565_scaled(rgb888, intensity_permille);
    int16_t half = (int16_t)(thickness / 2);
    if (half < 2) {
        half = 2;
    }

    for (int i = 0; i < segs; i++) {
        int deg = (i * 360) / segs;
        if (sweep) {
            int diff = deg - (int)sweep_deg;
            diff %= 360;
            if (diff < 0) {
                diff += 360;
            }
            if (diff > span_deg && diff < (360 - span_deg)) {
                continue;
            }
        }
        int32_t x = (int32_t)cx + ((int32_t)r_mid * cos_permille(deg)) / 1000;
        int32_t y = (int32_t)cy + ((int32_t)r_mid * sin_permille(deg)) / 1000;
        zv_status_t err = fill(c, (int16_t)(x - half), (int16_t)(y - half), (int16_t)(x + half + 1),
                               (int16_t)(y + half + 1), color);
        if (err != ZV_OK) {
            return err;
        }
    }
    return ZV_OK;
}

/*
 * Multi-band luminous ring: dim track, soft halo, mid glow, bright core, and a
 * comet head on the sweep arc. Intensity is scaled per band; no allocation.
 */
static zv_status_t paint_soft_ring(zv_ui_canvas_t *c, int16_t cx, int16_t cy, int16_t r_mid,
                                   uint32_t rgb888, uint16_t intensity_permille, bool sweep, uint16_t sweep_deg)
{
    /* Continuous annulus, with an open crown so clock and Z remain unobstructed. */
    const int16_t outer = (int16_t)(r_mid + 3), inner = (int16_t)(r_mid - 1);
    const uint16_t halo = zv_rgb888_to_rgb565_scaled(rgb888, (uint16_t)(intensity_permille / 4u));
    const uint16_t core = zv_rgb888_to_rgb565_scaled(rgb888, intensity_permille);
    uint16_t row[248];
    for (int16_t dy = (int16_t)(-r_mid + 28); dy < outer; dy++) {
        int16_t a = 0, b = 0;
        for (int16_t x = outer; x > 0; x--) {
            if ((int32_t)x * x + (int32_t)dy * dy <= (int32_t)outer * outer) { a = x; break; }
        }
        for (int16_t x = inner; x > 0; x--) {
            if ((int32_t)x * x + (int32_t)dy * dy <= (int32_t)inner * inner) { b = x; break; }
        }
        if (a == 0) continue;
        const int16_t span = (int16_t)(a + 2);
        const int16_t width = (int16_t)(span * 2);
        for (int16_t x = (int16_t)-span; x < span; x++) {
            int16_t distance = x < 0 ? (int16_t)-x : x;
            row[x + span] = distance >= b ? (distance <= a ? core : halo)
                                         : zv_rgb888_to_rgb565(ZV_COLOR_BACKGROUND);
        }
        if (c->draw_row != NULL) {
            zv_status_t err = c->draw_row(c->ctx, (int16_t)(cx - span), (int16_t)(cy + dy), (uint16_t)width, row);
            if (err != ZV_OK) return err;
        } else {
            for (int16_t x = 0; x < width;) {
                int16_t next = (int16_t)(x + 1);
                while (next < width && row[next] == row[x]) next++;
                zv_status_t err = fill(c, (int16_t)(cx - span + x), (int16_t)(cy + dy),
                                      (int16_t)(cx - span + next), (int16_t)(cy + dy + 1), row[x]);
                if (err != ZV_OK) return err;
                x = next;
            }
        }
    }
    if (sweep) return paint_ring(c, cx, cy, r_mid, 5, rgb888, 1000u, true, sweep_deg);
    return ZV_OK;
}

static zv_status_t paint_soft_ring_legacy(zv_ui_canvas_t *c, int16_t cx, int16_t cy, int16_t r_mid,
                                   uint32_t rgb888, uint16_t intensity_permille, bool sweep, uint16_t sweep_deg)
{
    static const struct {
        int16_t thickness;
        uint16_t scale;
    } k_bands[] = {
        /*
         * Two bands keep the neon silhouette while reserving enough DMA
         * headroom for date/time, battery, mic privacy and state effects.
         */
        {18, 360}, /* halo */
        {5, 1000}, /* core */
    };
    for (size_t i = 0; i < sizeof(k_bands) / sizeof(k_bands[0]); i++) {
        uint16_t scaled =
            (uint16_t)(((uint32_t)intensity_permille * k_bands[i].scale) / 1000u);
        if (scaled == 0u && k_bands[i].scale > 0u) {
            scaled = 1u;
        }
        zv_status_t err = paint_ring(c, cx, cy, r_mid, k_bands[i].thickness, rgb888, scaled, sweep, sweep_deg);
        if (err != ZV_OK) {
            return err;
        }
    }
    if (sweep) {
        /* Brighter, slightly thicker head so the arc reads as motion. */
        zv_status_t err = paint_ring(c, cx, cy, r_mid, 8, rgb888, intensity_permille, true, sweep_deg);
        if (err != ZV_OK) {
            return err;
        }
    }
    return ZV_OK;
}


zv_status_t zv_ui_paint_text(zv_ui_canvas_t *canvas, int16_t x, int16_t y, const char *text, uint16_t color,
                             uint8_t scale)
{
    if (canvas == NULL || text == NULL || scale == 0u) {
        return ZV_ERR_INVALID_ARG;
    }
    int16_t pen = x;
    for (const char *p = text; *p != '\0'; p++) {
        /* signed char makes `ch > 127` a tautology under -Wtype-limits */
        unsigned char ch = (unsigned char)*p;
        if (ch >= (unsigned char)'a' && ch <= (unsigned char)'z') {
            ch = (unsigned char)(ch - 'a' + 'A');
        }
        if (ch < 32u || ch > 127u) {
            ch = (unsigned char)'?';
        }
        const uint8_t *glyph = k_font[ch - 32];
        for (int col = 0; col < ZV_UI_FONT_W; col++) {
            uint8_t bits = glyph[col];
            int row = 0;
            while (row < ZV_UI_FONT_H) {
                if ((bits & (uint8_t)(1u << row)) == 0u) {
                    row++;
                    continue;
                }
                int run = row;
                while (run < ZV_UI_FONT_H && (bits & (uint8_t)(1u << run)) != 0u) {
                    run++;
                }
                int16_t py = (int16_t)(y + row * scale);
                int16_t ph = (int16_t)((run - row) * scale);
                int16_t px = (int16_t)(pen + col * scale);
                zv_status_t err = fill(canvas, px, py, (int16_t)(px + scale), (int16_t)(py + ph), color);
                if (err != ZV_OK) {
                    return err;
                }
                row = run;
            }
        }
        pen = (int16_t)(pen + (ZV_UI_FONT_W + 1) * scale);
    }
    return ZV_OK;
}

int16_t zv_ui_text_width(const char *text, uint8_t scale)
{
    if (text == NULL || scale == 0u) {
        return 0;
    }
    size_t n = strlen(text);
    if (n == 0u) {
        return 0;
    }
    return (int16_t)(n * (ZV_UI_FONT_W + 1) * scale);
}

static zv_status_t paint_wave(zv_ui_canvas_t *c, const zv_ui_chrome_t *chrome, uint16_t color, uint16_t dim)
{
    if (chrome == NULL || !chrome->show_wave || chrome->wave_len == 0u) return ZV_OK;
    size_t count=chrome->wave_len;
    if (count > (size_t)ZV_UI_WAVE_BARS) count=(size_t)ZV_UI_WAVE_BARS;
    int32_t level=0;
    for (size_t i=0; i<count; ++i) {
        int32_t value=chrome->wave[i];
        if (value<0) value=-value;
        if (value>level) level=value;
    }
    if (level>1000) level=1000;
    /* One energy meter: mirrored weights, not invented frequency bands.
     * Entire geometry remains inside the existing y=266..279 patch. */
    static const int16_t weight[7]={350,550,800,1000,800,550,350};
    for (int16_t i=0; i<7; ++i) {
        int16_t height=(int16_t)(3+(level*weight[i]*10)/1000000);
        int16_t x=(int16_t)(147+i*10);
        int16_t top=(int16_t)(273-height/2);
        uint16_t ink=level>0 ? color:dim;
        zv_status_t err=fill(c,(int16_t)(x+1),top,(int16_t)(x+4),(int16_t)(top+1),ink);
        if (err!=ZV_OK) return err;
        err=fill(c,x,(int16_t)(top+1),(int16_t)(x+5),(int16_t)(top+height-1),ink);
        if (err!=ZV_OK) return err;
        err=fill(c,(int16_t)(x+1),(int16_t)(top+height-1),(int16_t)(x+4),(int16_t)(top+height),ink);
        if (err!=ZV_OK) return err;
    }
    return ZV_OK;
}

static zv_status_t paint_chip(zv_ui_canvas_t *c, int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint32_t rgb888,
                              uint16_t border_intensity, uint16_t fill_mix)
{
    /* fill_mix 0..1000 scales navy toward the ring colour for a tinted panel. */
    uint32_t base = ZV_COLOR_NAVY;
    uint32_t tint = rgb888;
    uint32_t r = (((base >> 16) & 0xFFu) * (1000u - fill_mix) + ((tint >> 16) & 0xFFu) * fill_mix) / 1000u;
    uint32_t g = (((base >> 8) & 0xFFu) * (1000u - fill_mix) + ((tint >> 8) & 0xFFu) * fill_mix) / 1000u;
    uint32_t b = ((base & 0xFFu) * (1000u - fill_mix) + (tint & 0xFFu) * fill_mix) / 1000u;
    uint16_t panel = zv_rgb_to_rgb565((uint8_t)r, (uint8_t)g, (uint8_t)b);
    zv_status_t err = paint_round_rect(c, x0, y0, x1, y1, 12, panel);
    if (err != ZV_OK) {
        return err;
    }
    /* 1px luminous border as an inset stroke of larger radius. */
    uint16_t border = zv_rgb888_to_rgb565_scaled(rgb888, border_intensity);
    for (int16_t dy = 0; dy < (int16_t)(y1 - y0); dy++) {
        int16_t inset = 0;
        int16_t h = (int16_t)(y1 - y0);
        int16_t radius = 12;
        if (dy < radius) {
            inset = corner_inset(dy, radius);
        } else if (dy >= (int16_t)(h - radius)) {
            inset = corner_inset((int16_t)(h - 1 - dy), radius);
        }
        /* top and bottom edges */
        if (dy == 0 || dy == (int16_t)(h - 1)) {
            err = fill(c, (int16_t)(x0 + inset), (int16_t)(y0 + dy), (int16_t)(x1 - inset),
                       (int16_t)(y0 + dy + 1), border);
            if (err != ZV_OK) {
                return err;
            }
            continue;
        }
        /* left and right edges */
        err = fill(c, (int16_t)(x0 + inset), (int16_t)(y0 + dy), (int16_t)(x0 + inset + 1),
                   (int16_t)(y0 + dy + 1), border);
        if (err != ZV_OK) {
            return err;
        }
        err = fill(c, (int16_t)(x1 - inset - 1), (int16_t)(y0 + dy), (int16_t)(x1 - inset),
                   (int16_t)(y0 + dy + 1), border);
        if (err != ZV_OK) {
            return err;
        }
    }
    return ZV_OK;
}

static zv_status_t paint_orbital_field(zv_ui_canvas_t *c, int16_t cx, int16_t cy, uint32_t rgb888)
{
    static const struct {
        int16_t x;
        int16_t y;
        uint8_t size;
        uint16_t glow;
    } k_nodes[] = {
        {-118, -76, 3, 360}, {116, -62, 2, 240}, {-136, 42, 2, 220}, {132, 58, 3, 340},
        {-82, 111, 2, 260},  {91, 106, 2, 220},  {-62, -126, 2, 200}, {70, -122, 2, 280},
    };
    uint16_t orbit = zv_rgb888_to_rgb565_scaled(rgb888, 125u);
    for (int deg = 0; deg < 360; deg += 12) {
        int16_t x = (int16_t)(cx + ((int32_t)122 * cos_permille(deg)) / 1000);
        int16_t y = (int16_t)(cy + ((int32_t)88 * sin_permille(deg)) / 1000);
        zv_status_t err = fill(c, x, y, (int16_t)(x + 2), (int16_t)(y + 2), orbit);
        if (err != ZV_OK) {
            return err;
        }
    }
    for (size_t i = 0; i < sizeof(k_nodes) / sizeof(k_nodes[0]); i++) {
        uint16_t node = zv_rgb888_to_rgb565_scaled(rgb888, k_nodes[i].glow);
        int16_t x = (int16_t)(cx + k_nodes[i].x);
        int16_t y = (int16_t)(cy + k_nodes[i].y);
        zv_status_t err = fill(c, (int16_t)(x - k_nodes[i].size), (int16_t)(y - k_nodes[i].size),
                               (int16_t)(x + k_nodes[i].size), (int16_t)(y + k_nodes[i].size), node);
        if (err != ZV_OK) {
            return err;
        }
    }
    return ZV_OK;
}

static zv_status_t paint_core(zv_ui_canvas_t *c, int16_t cx, int16_t cy, uint32_t rgb888)
{
    static const struct {
        int16_t radius;
        uint16_t intensity;
    } k_layers[] = {{92, 35}, {84, 55}, {75, 80}};
    /*
     * The LCD canvas turns every fill into a synchronous DMA transaction.
     * A scanline ellipse would therefore cost almost 500 transfers for these
     * three layers. Nine horizontal bands per layer retain the faceted orb
     * silhouette while bounding the core to 27 fills and zero allocation.
     */
    enum { CORE_BANDS = 9 };
    for (size_t i = 0; i < sizeof(k_layers) / sizeof(k_layers[0]); i++) {
        const int16_t radius = k_layers[i].radius;
        const int16_t diameter = (int16_t)(radius * 2);
        const uint16_t color = zv_rgb888_to_rgb565_scaled(rgb888, k_layers[i].intensity);
        for (int band = 0; band < CORE_BANDS; band++) {
            const int16_t y0 = (int16_t)(cy - radius + ((int32_t)diameter * band) / CORE_BANDS);
            const int16_t y1 = (int16_t)(cy - radius + ((int32_t)diameter * (band + 1)) / CORE_BANDS);
            const int16_t sample_y = (int16_t)(((int32_t)y0 + y1) / 2 - cy);
            const int32_t remaining = (int32_t)radius * radius - (int32_t)sample_y * sample_y;
            int16_t span = 0;
            for (int16_t x = radius; x > 0; x--) {
                if ((int32_t)x * x <= remaining) {
                    span = x;
                    break;
                }
            }
            zv_status_t err = fill(c, (int16_t)(cx - span), y0, (int16_t)(cx + span), y1, color);
            if (err != ZV_OK) {
                return err;
            }
        }
    }
    /* Four cardinal ticks make the face read as an instrument, not an emoji. */
    uint16_t tick = zv_rgb888_to_rgb565_scaled(rgb888, 520u);
    static const int16_t k_ticks[][4] = {
        {-2, -105, 2, -94}, {-2, 94, 2, 105}, {-105, -2, -94, 2}, {94, -2, 105, 2},
    };
    for (size_t i = 0; i < sizeof(k_ticks) / sizeof(k_ticks[0]); i++) {
        zv_status_t err = fill(c, (int16_t)(cx + k_ticks[i][0]), (int16_t)(cy + k_ticks[i][1]),
                               (int16_t)(cx + k_ticks[i][2]), (int16_t)(cy + k_ticks[i][3]), tick);
        if (err != ZV_OK) {
            return err;
        }
    }
    return ZV_OK;
}

static zv_status_t paint_connectivity(zv_ui_canvas_t *c, const zv_ui_chrome_t *chrome, uint32_t rgb888)
{
    (void)rgb888;
    if (chrome == NULL || !chrome->show_connectivity) return ZV_OK;
    const uint16_t cyan = zv_rgb888_to_rgb565(0x20DFFFu);
    const uint16_t dim = zv_rgb888_to_rgb565(0x33495Du);
    const uint16_t ink = zv_rgb888_to_rgb565(0xEDF7FFu);
    const uint8_t bars = chrome->signal_bars > 3u ? 3u : chrome->signal_bars;
    /* Three small smooth Wi-Fi arcs at the left of the clock. */
    for (int band = 0; band < 3; band++) {
        int radius = 5 + band * 6;
        uint16_t color = bars > (uint8_t)band ? cyan : dim;
        for (int deg = 225; deg <= 315; deg += 8) {
            int16_t x = (int16_t)(73 + radius * cos_permille(deg) / 1000);
            int16_t y = (int16_t)(77 + radius * sin_permille(deg) / 1000);
            zv_status_t err = fill(c, x, y, (int16_t)(x + 3), (int16_t)(y + 3), color);
            if (err != ZV_OK) return err;
        }
    }
    zv_status_t err = paint_ellipse(c, 74, 79, 3, 3, bars ? cyan : dim);
    if (err != ZV_OK) return err;
    /* The lock is lit only for an authenticated session. */
    uint16_t lock = chrome->secure_link ? ink : dim;
    err = paint_round_rect(c, 258, 67, 274, 81, 3, lock);
    if (err != ZV_OK) return err;
    err = paint_ellipse(c, 266, 66, 6, 8, lock);
    if (err != ZV_OK) return err;
    err = paint_ellipse(c, 266, 66, 3, 5, zv_rgb888_to_rgb565(ZV_COLOR_BACKGROUND));
    if (err != ZV_OK) return err;
    err = fill(c, 258, 70, 274, 80, lock);
    if (err != ZV_OK) return err;
    return fill(c, 265, 73, 268, 78, zv_rgb888_to_rgb565(ZV_COLOR_BACKGROUND));
}

/** Battery shell + fill; unknown reading paints an empty shell with a dash. */
static zv_status_t paint_battery(zv_ui_canvas_t *c, const zv_ui_chrome_t *chrome, uint32_t rgb888)
{
    (void)rgb888;
    if (chrome == NULL || !chrome->show_connectivity) return ZV_OK;
    const uint16_t ink = zv_rgb888_to_rgb565(0xEDF7FFu);
    const uint16_t dim = zv_rgb888_to_rgb565(0x33495Du);
    const uint16_t shell = chrome->battery_ok ? ink : dim;
    zv_status_t err = paint_round_rect(c, 278, 62, 309, 78, 3, shell);
    if (err != ZV_OK) return err;
    err = fill(c, 281, 65, 306, 75, zv_rgb888_to_rgb565(ZV_COLOR_BACKGROUND));
    if (err != ZV_OK) return err;
    err = fill(c, 309, 67, 312, 73, shell);
    if (err != ZV_OK) return err;
    char percent[5] = "--";
    if (chrome->battery_ok) {
        uint8_t pct = chrome->battery_percent > 100u ? 100u : chrome->battery_percent;
        uint16_t color = zv_rgb888_to_rgb565(0x24F6A4u);
        if (pct <= ZV_UI_BATTERY_CRITICAL_PCT) color = zv_rgb888_to_rgb565(ZV_COLOR_ERROR);
        else if (pct <= ZV_UI_BATTERY_LOW_PCT) color = zv_rgb888_to_rgb565(ZV_COLOR_APPROVAL);
        err = fill(c, 282, 66, (int16_t)(282 + (int32_t)23 * pct / 100), 74, color);
        if (err != ZV_OK) return err;
        if (pct >= 100u) { memcpy(percent, "100%", 5u); }
        else if (pct >= 10u) { percent[0] = (char)('0' + pct / 10u); percent[1] = (char)('0' + pct % 10u); percent[2] = '%'; percent[3] = '\0'; }
        else { percent[0] = (char)('0' + pct); percent[1] = '%'; percent[2] = '\0'; }
        if (pct <= ZV_UI_BATTERY_CRITICAL_PCT) {
            err = fill(c, 269, 65, 272, 72, color);
            if (err != ZV_OK) return err;
            err = fill(c, 269, 74, 272, 77, color);
            if (err != ZV_OK) return err;
        }
    }
    return zv_ui_paint_text(c, (int16_t)(295 - zv_ui_text_width(percent, 1u) / 2), 84, percent, shell, 1u);
}

/** True for screens that own the whole panel and must not draw the corner plates. */
static bool screen_is_overlay(zv_ui_screen_t screen)
{
    return screen == ZV_UI_SCREEN_MENU || screen == ZV_UI_SCREEN_VOLUME ||
           screen == ZV_UI_SCREEN_CONFIRM || screen == ZV_UI_SCREEN_NETWORK;
}

/*
 * Hit testing lives beside the painter so the pixels the operator can see and
 * the pixels the shell treats as tappable cannot drift apart.
 */
bool zv_ui_home_hit(int16_t x, int16_t y)
{
    return x >= ZV_UI_SHELL_HOME_TOUCH_X0 && x < ZV_UI_SHELL_HOME_TOUCH_X1 &&
           y >= ZV_UI_SHELL_BOTTOM_TOUCH_Y0 && y < ZV_UI_SHELL_BOTTOM_TOUCH_Y1;
}

bool zv_ui_volume_hit(int16_t x, int16_t y)
{
    return x >= ZV_UI_SHELL_VOLUME_TOUCH_X0 && x < ZV_UI_SHELL_VOLUME_TOUCH_X1 &&
           y >= ZV_UI_SHELL_BOTTOM_TOUCH_Y0 && y < ZV_UI_SHELL_BOTTOM_TOUCH_Y1;
}

bool zv_ui_menu_hit(int16_t x, int16_t y, uint8_t index_limit, uint8_t *index_out)
{
    if (index_out == NULL || index_limit == 0u || index_limit > (uint8_t)ZV_UI_MAX_MENU) {
        return false;
    }
    if (x < ZV_UI_MENU_X0 || x > ZV_UI_MENU_X1 || y < ZV_UI_MENU_Y0) {
        return false;
    }
    /* Rows run past the last one for whatever vertical space is left over, so
     * the tail of the panel is not a dead zone that looks tappable. */
    int32_t index = ((int32_t)y - (int32_t)ZV_UI_MENU_Y0) / (int32_t)ZV_UI_MENU_ITEM_H;
    if (index < 0 || index >= (int32_t)index_limit) {
        return false;
    }
    *index_out = (uint8_t)index;
    return true;
}

bool zv_ui_confirm_hit_yes(int16_t x, int16_t y)
{
    return x >= ZV_UI_CONFIRM_YES_X0 && x <= ZV_UI_CONFIRM_YES_X1 && y >= ZV_UI_CONFIRM_YES_Y0 &&
           y <= ZV_UI_CONFIRM_YES_Y1;
}

bool zv_ui_confirm_hit_no(int16_t x, int16_t y)
{
    return x >= ZV_UI_CONFIRM_NO_X0 && x <= ZV_UI_CONFIRM_NO_X1 && y >= ZV_UI_CONFIRM_NO_Y0 &&
           y <= ZV_UI_CONFIRM_NO_Y1;
}

bool zv_ui_volume_bar_hit(int16_t x, int16_t y)
{
    return x >= ZV_UI_VOLUME_BAR_X0 && x <= ZV_UI_VOLUME_BAR_X1 && y >= ZV_UI_VOLUME_BAR_Y0 &&
           y <= ZV_UI_VOLUME_BAR_Y1;
}

bool zv_ui_volume_mute_hit(int16_t x, int16_t y)
{
    return x >= ZV_UI_VOLUME_MUTE_X0 && x <= ZV_UI_VOLUME_MUTE_X1 &&
           y >= ZV_UI_VOLUME_MUTE_Y0 && y <= ZV_UI_VOLUME_MUTE_Y1;
}

bool zv_ui_volume_accept_hit(int16_t x, int16_t y)
{
    return x >= ZV_UI_VOLUME_ACCEPT_X0 && x <= ZV_UI_VOLUME_ACCEPT_X1 &&
           y >= ZV_UI_VOLUME_ACCEPT_Y0 && y <= ZV_UI_VOLUME_ACCEPT_Y1;
}

uint8_t zv_ui_volume_from_x(int16_t x)
{
    if (x <= ZV_UI_VOLUME_BAR_X0) {
        return 0u;
    }
    if (x >= ZV_UI_VOLUME_BAR_X1) {
        return (uint8_t)ZV_VOLUME_MAX;
    }
    int32_t span = (int32_t)ZV_UI_VOLUME_BAR_X1 - (int32_t)ZV_UI_VOLUME_BAR_X0;
    int32_t offset = (int32_t)x - (int32_t)ZV_UI_VOLUME_BAR_X0;
    if (span <= 0) {
        return 0u;
    }
    return (uint8_t)(((offset * (int32_t)ZV_VOLUME_MAX) + (span / 2)) / span);
}

const char *zv_ui_screen_name(zv_ui_screen_t screen)
{
    switch (screen) {
    case ZV_UI_SCREEN_MENU:
        return "menu";
    case ZV_UI_SCREEN_VOLUME:
        return "volume";
    case ZV_UI_SCREEN_NETWORK:
        return "network";
    case ZV_UI_SCREEN_CONFIRM:
        return "confirm";
    case ZV_UI_SCREEN_HOME:
    default:
        return "home";
    }
}

/**
 * Bottom-left volume / mute plate, mirroring the home affordance on the opposite
 * corner. The ten segments come from zv_volume_bars(), so the plate cannot show a
 * level that disagrees with the level actually sent to the amplifier.
 */
static void format_percent(char *buf, size_t len, uint8_t percent);

static zv_status_t paint_volume_indicator(zv_ui_canvas_t *c, const zv_ui_chrome_t *chrome, uint32_t rgb888)
{
    if (chrome == NULL || !chrome->show_volume || screen_is_overlay(chrome->screen)) {
        return ZV_OK;
    }
    uint32_t plate_rgb = chrome->volume.muted ? ZV_COLOR_ERROR : rgb888;
    uint16_t plate = zv_rgb888_to_rgb565_scaled(plate_rgb, 220u);
    uint16_t ink = zv_rgb888_to_rgb565_scaled(0xD9F2FFu, 860u);
    uint16_t dim = zv_rgb888_to_rgb565_scaled(0xD9F2FFu, 150u);
    zv_status_t err = fill(c, ZV_UI_VOLUME_X0, ZV_UI_VOLUME_Y0, ZV_UI_VOLUME_X1, ZV_UI_VOLUME_Y1, plate);
    if (err != ZV_OK) {
        return err;
    }
    const int16_t cy = (int16_t)((ZV_UI_VOLUME_Y0 + ZV_UI_VOLUME_Y1) / 2 - 5);

    if (chrome->volume.muted) {
        /* Muted reads as a word rather than as an empty bar: an empty bar and a
         * zero volume look identical, and only one of them is silence. */
        int16_t tw = zv_ui_text_width("MUTE", 1);
        return zv_ui_paint_text(c, (int16_t)((ZV_UI_VOLUME_X0 + ZV_UI_VOLUME_X1 - tw) / 2),
                                (int16_t)(cy - 3), "MUTE", ink, 1);
    }

    /* Speaker body and cone. */
    err = fill(c, ZV_UI_VOLUME_X0 + 6, (int16_t)(cy - 4), ZV_UI_VOLUME_X0 + 10, (int16_t)(cy + 4), ink);
    if (err != ZV_OK) {
        return err;
    }
    err = fill(c, ZV_UI_VOLUME_X0 + 10, (int16_t)(cy - 7), ZV_UI_VOLUME_X0 + 13, (int16_t)(cy + 7), ink);
    if (err != ZV_OK) {
        return err;
    }

    uint8_t bars = zv_volume_bars(&chrome->volume);
    for (uint8_t i = 0; i < (uint8_t)ZV_VOLUME_BARS; i++) {
        int16_t h = (int16_t)(2 + (int16_t)i / 2);
        int16_t bx = (int16_t)(ZV_UI_VOLUME_X0 + 17 + (int16_t)i * 2);
        int16_t y0 = (int16_t)(cy - h / 2);
        err = fill(c, bx, y0, (int16_t)(bx + 1), (int16_t)(y0 + h), i < bars ? ink : dim);
        if (err != ZV_OK) {
            return err;
        }
    }
    char percent[5];
    format_percent(percent, sizeof(percent), zv_volume_effective(&chrome->volume));
    return zv_ui_paint_text(c, (int16_t)(126-zv_ui_text_width(percent,1u)/2), 334, percent, ink, 1u);
}

/** Renders 0..100 as "NN%" without pulling stdio into the portable painter. */
static void format_percent(char *buf, size_t len, uint8_t percent)
{
    if (buf == NULL || len < 5u) return;
    uint8_t pct = percent > (uint8_t)ZV_VOLUME_MAX ? (uint8_t)ZV_VOLUME_MAX : percent;
    size_t at = 0u;
    if (pct == 100u) buf[at++] = '1';
    if (pct >= 10u) buf[at++] = (char)('0' + (pct / 10u) % 10u);
    buf[at++] = (char)('0' + pct % 10u);
    buf[at++] = '%';
    buf[at] = '\0';
}

/** Read-only authenticated-session status with the shared, finger-sized BACK target. */
static zv_status_t paint_network_screen(zv_ui_canvas_t *c, const zv_ui_chrome_t *chrome, uint32_t rgb888)
{
    const uint16_t ink = zv_rgb888_to_rgb565(0xD9F2FFu);
    zv_status_t err = paint_round_rect(c, 40, 120, 320, 300, 12,
                                      zv_rgb888_to_rgb565(ZV_COLOR_NAVY));
    if (err != ZV_OK) return err;
    const char *status = chrome->detail_text != NULL ? chrome->detail_text : "NOT CHECKED";
    /* Keep arbitrary caller text within the round panel. */
    if (strlen(status) > 20u) return ZV_ERR_INVALID_ARG;
    err = zv_ui_paint_text(c, 138, 140, "NETWORK", ink, 2);
    if (err != ZV_OK) return err;
    err = zv_ui_paint_text(c, (int16_t)((ZV_UI_W-zv_ui_text_width(status,2))/2),
                           184, status, ink, 2);
    if (err != ZV_OK) return err;
    err = zv_ui_paint_text(c, 141, 218, "VOICE SESSION", ink, 1);
    if (err != ZV_OK) return err;
    err = paint_round_rect(c, ZV_UI_VOLUME_ACCEPT_X0, ZV_UI_VOLUME_ACCEPT_Y0,
                           ZV_UI_VOLUME_ACCEPT_X1+1, ZV_UI_VOLUME_ACCEPT_Y1+1, 6,
                           zv_rgb888_to_rgb565_scaled(rgb888,250u));
    if (err != ZV_OK) return err;
    return zv_ui_paint_text(c, 156, 278, "BACK", ink, 2);
}

/** Full-panel volume slider. The track is drawn from the shared hit constants. */
static zv_status_t paint_volume_screen(zv_ui_canvas_t *c, const zv_ui_chrome_t *chrome, uint32_t rgb888)
{
    uint16_t ink = zv_rgb888_to_rgb565(0xD9F2FFu);
    uint16_t dim_ink = zv_rgb888_to_rgb565_scaled(0xD9F2FFu, 600u);
    char pct_text[5];
    format_percent(pct_text, sizeof(pct_text), zv_volume_level(&chrome->volume));

    /* Cover the underlying status chip as well as the controls. */
    zv_status_t err = paint_round_rect(c, 40, 120, (int16_t)(ZV_UI_W - 40), 320, 12,
                                       zv_rgb888_to_rgb565(ZV_COLOR_NAVY));
    if (err != ZV_OK) return err;

    int16_t tw = zv_ui_text_width("VOLUME", 2);
    err = zv_ui_paint_text(c, (int16_t)((ZV_UI_W - tw) / 2), 140, "VOLUME", dim_ink, 2);
    if (err != ZV_OK) return err;

    /* While muted the number stays visible but dimmed, so the operator can see
     * what they will get back rather than a blank panel. */
    uint16_t pct_ink = chrome->volume.muted ? dim_ink : ink;
    tw = zv_ui_text_width(pct_text, 3);
    err = zv_ui_paint_text(c, (int16_t)((ZV_UI_W - tw) / 2), 186, pct_text, pct_ink, 3);
    if (err != ZV_OK) return err;

    err = fill(c, ZV_UI_VOLUME_BAR_X0, ZV_UI_VOLUME_BAR_Y0, (int16_t)(ZV_UI_VOLUME_BAR_X1 + 1),
               (int16_t)(ZV_UI_VOLUME_BAR_Y1 + 1), zv_rgb888_to_rgb565_scaled(0xD9F2FFu, 110u));
    if (err != ZV_OK) return err;
    if (!chrome->volume.muted && chrome->volume.level > 0u) {
        int32_t span = (int32_t)ZV_UI_VOLUME_BAR_X1 - (int32_t)ZV_UI_VOLUME_BAR_X0;
        int32_t filled = (span * (int32_t)chrome->volume.level) / (int32_t)ZV_VOLUME_MAX;
        err = fill(c, ZV_UI_VOLUME_BAR_X0, ZV_UI_VOLUME_BAR_Y0,
                   (int16_t)(ZV_UI_VOLUME_BAR_X0 + filled), (int16_t)(ZV_UI_VOLUME_BAR_Y1 + 1),
                   zv_rgb888_to_rgb565_scaled(rgb888, 820u));
        if (err != ZV_OK) return err;
    }

    const char *mute_text = chrome->volume.muted ? "MUTE: ON" : "MUTE: OFF";
    err = paint_round_rect(c, ZV_UI_VOLUME_MUTE_X0, ZV_UI_VOLUME_MUTE_Y0,
                          ZV_UI_VOLUME_MUTE_X1 + 1, ZV_UI_VOLUME_MUTE_Y1 + 1, 6,
                          zv_rgb888_to_rgb565_scaled(chrome->volume.muted ? ZV_COLOR_ERROR : rgb888, 250u));
    if (err != ZV_OK) return err;
    tw = zv_ui_text_width(mute_text, 2);
    err = zv_ui_paint_text(c, (int16_t)((ZV_UI_W - tw) / 2), 220, mute_text, ink, 2);
    if (err != ZV_OK) return err;
    err = paint_round_rect(c, ZV_UI_VOLUME_ACCEPT_X0, ZV_UI_VOLUME_ACCEPT_Y0,
                           ZV_UI_VOLUME_ACCEPT_X1 + 1, ZV_UI_VOLUME_ACCEPT_Y1 + 1, 6,
                           zv_rgb888_to_rgb565_scaled(rgb888, 250u));
    if (err != ZV_OK) return err;
    tw = zv_ui_text_width("ACCEPT", 2);
    err = zv_ui_paint_text(c, (int16_t)((ZV_UI_W - tw) / 2), 278, "ACCEPT", ink, 2);
    return err;
}

/**
 * Restart confirmation. This screen is the only path to esp_restart(); it exists
 * so a stray tap cannot bounce the device under the operator mid-task.
 */
static zv_status_t paint_confirm_screen(zv_ui_canvas_t *c, uint32_t rgb888)
{
    uint16_t ink = zv_rgb888_to_rgb565(0xD9F2FFu);
    uint16_t dim_ink = zv_rgb888_to_rgb565_scaled(0xD9F2FFu, 700u);
    uint16_t yes_fill = zv_rgb888_to_rgb565_scaled(ZV_COLOR_ERROR, 700u);
    uint16_t no_fill = zv_rgb888_to_rgb565_scaled(0xD9F2FFu, 160u);
    zv_status_t err = paint_round_rect(c, 40, 100, (int16_t)(ZV_UI_W - 40), 310, 12,
                                       zv_rgb888_to_rgb565(ZV_COLOR_NAVY));
    if (err != ZV_OK) return err;

    int16_t tw = zv_ui_text_width("ARE YOU SURE?", 2);
    err = zv_ui_paint_text(c, (int16_t)((ZV_UI_W - tw) / 2), 140, "ARE YOU SURE?", ink, 2);
    if (err != ZV_OK) return err;
    tw = zv_ui_text_width("RESTART DEVICE?", 2);
    err = zv_ui_paint_text(c, (int16_t)((ZV_UI_W - tw) / 2), 174, "RESTART DEVICE?", dim_ink, 2);
    if (err != ZV_OK) return err;
    tw = zv_ui_text_width("ANY IN-PROGRESS WORK IS LOST", 1);
    err = zv_ui_paint_text(c, (int16_t)((ZV_UI_W - tw) / 2), 204, "ANY IN-PROGRESS WORK IS LOST", dim_ink, 1);
    if (err != ZV_OK) return err;
    /* Accent rule in the state colour, matching the menu header. */
    err = fill(c, 120, 222, (int16_t)(ZV_UI_W - 120), 225, zv_rgb888_to_rgb565_scaled(rgb888, 700u));
    if (err != ZV_OK) return err;

    /* Buttons are drawn one pixel past their inclusive hit rect so the lit
     * area and the tappable area are the same area. */
    err = fill(c, ZV_UI_CONFIRM_YES_X0, ZV_UI_CONFIRM_YES_Y0, (int16_t)(ZV_UI_CONFIRM_YES_X1 + 1),
               (int16_t)(ZV_UI_CONFIRM_YES_Y1 + 1), yes_fill);
    if (err != ZV_OK) return err;
    err = fill(c, ZV_UI_CONFIRM_NO_X0, ZV_UI_CONFIRM_NO_Y0, (int16_t)(ZV_UI_CONFIRM_NO_X1 + 1),
               (int16_t)(ZV_UI_CONFIRM_NO_Y1 + 1), no_fill);
    if (err != ZV_OK) return err;

    const int16_t btn_h = (int16_t)(ZV_UI_CONFIRM_YES_Y1 - ZV_UI_CONFIRM_YES_Y0);
    const int16_t text_y = (int16_t)(ZV_UI_CONFIRM_YES_Y0 + (btn_h - 14) / 2);
    tw = zv_ui_text_width("YES", 2);
    err = zv_ui_paint_text(c, (int16_t)(ZV_UI_CONFIRM_YES_X0 + (ZV_UI_CONFIRM_YES_X1 - ZV_UI_CONFIRM_YES_X0 - tw) / 2),
                           text_y, "YES", ink, 2);
    if (err != ZV_OK) return err;
    tw = zv_ui_text_width("NO", 2);
    return zv_ui_paint_text(c, (int16_t)(ZV_UI_CONFIRM_NO_X0 + (ZV_UI_CONFIRM_NO_X1 - ZV_UI_CONFIRM_NO_X0 - tw) / 2),
                            text_y, "NO", ink, 2);
}

/** Bottom-left microphone/privacy cue. Muted draws a slash through the mic. */
static zv_status_t paint_mic(zv_ui_canvas_t *c, const zv_ui_chrome_t *chrome, uint32_t rgb888)
{
    if (chrome == NULL || !chrome->show_mic || chrome->menu_open) return ZV_OK;
    uint16_t ink = chrome->mic_muted ? zv_rgb888_to_rgb565(ZV_COLOR_ERROR)
                                     : zv_rgb888_to_rgb565_scaled(rgb888, 760u);
    uint16_t plate = zv_rgb888_to_rgb565_scaled(rgb888, 160u);
    zv_status_t err = paint_ellipse(c, 126, 329, 18, 18, plate);
    if (err != ZV_OK) return err;
    /* Mic capsule: slim rounded head so it reads apart from the cradle. */
    err = paint_round_rect(c, 122, 318, 130, 330, 4, ink);
    if (err != ZV_OK) return err;
    /* Cradle arms, kept 2px clear of the capsule so the shapes separate. */
    err = fill(c, 117, 325, 120, 332, ink);
    if (err != ZV_OK) return err;
    err = fill(c, 132, 325, 135, 332, ink);
    if (err != ZV_OK) return err;
    /* Cradle bottom bar and stem. */
    err = fill(c, 117, 332, 135, 335, ink);
    if (err != ZV_OK) return err;
    err = fill(c, 124, 335, 128, 340, ink);
    if (err != ZV_OK) return err;
    if (chrome->mic_muted) {
        /* Diagonal privacy slash across the button. */
        for (int i = 0; i < 7; i++) {
            err = fill(c, (int16_t)(112 + i * 3), (int16_t)(316 + i * 3),
                       (int16_t)(115 + i * 3), (int16_t)(319 + i * 3), ink);
            if (err != ZV_OK) return err;
        }
    }
    return ZV_OK;
}

/** Bottom-right home / menu affordance: 36px circle centred at (234,329),
 * fully inside the round display. */

static zv_status_t paint_home(zv_ui_canvas_t *c, const zv_ui_chrome_t *chrome, uint32_t rgb888)
{
    if (chrome == NULL || !chrome->show_home || chrome->menu_open || screen_is_overlay(chrome->screen)) return ZV_OK;
    zv_status_t err = paint_ellipse(c, 234, 329, 18, 18, zv_rgb888_to_rgb565_scaled(rgb888, 250u));
    if (err != ZV_OK) return err;
    const uint16_t ink = zv_rgb888_to_rgb565(0xEDF7FFu);
    for (int i = 0; i < 3; i++) {
        err = fill(c, 226, (int16_t)(323 + i * 5), 242, (int16_t)(325 + i * 5), ink);
        if (err != ZV_OK) return err;
    }
    return ZV_OK;
}

static zv_status_t paint_cancel(zv_ui_canvas_t *c, const zv_ui_chrome_t *chrome)
{
    if (chrome == NULL || !chrome->show_cancel || chrome->menu_open || screen_is_overlay(chrome->screen))
        return ZV_OK;
    uint16_t red=zv_rgb888_to_rgb565(ZV_COLOR_ERROR);
    zv_status_t err=paint_ellipse(c,180,329,16,16,red);
    if (err != ZV_OK) return err;
    err=paint_ellipse(c,180,329,12,12,zv_rgb888_to_rgb565(ZV_COLOR_NAVY));
    if (err != ZV_OK) return err;
    for (int i=-9; i<=9; ++i) {
        err=fill(c,(int16_t)(179+i),(int16_t)(328-i),(int16_t)(182+i),(int16_t)(331-i),red);
        if (err != ZV_OK) return err;
    }
    return zv_ui_paint_text(c,162,349,"CANCEL",red,1u);
}

/* zv_link_state_name() returns lowercase; status cues must not depend on case. */
static bool status_is(const char *text, const char *want)
{
    if (text == NULL || want == NULL) {
        return false;
    }
    while (*text != '\0' && *want != '\0') {
        unsigned char a = (unsigned char)*text++;
        unsigned char b = (unsigned char)*want++;
        if (a >= (unsigned char)'a' && a <= (unsigned char)'z') {
            a = (unsigned char)(a - (unsigned char)'a' + (unsigned char)'A');
        }
        if (b >= (unsigned char)'a' && b <= (unsigned char)'z') {
            b = (unsigned char)(b - (unsigned char)'a' + (unsigned char)'A');
        }
        if (a != b) {
            return false;
        }
    }
    return *text == '\0' && *want == '\0';
}

static zv_status_t paint_state_fx(zv_ui_canvas_t *c, zv_avatar_state_t state, const zv_ui_chrome_t *chrome,
                                  int16_t cx, int16_t cy, const zv_face_frame_t *face)
{
    const uint32_t rgb888 = face->ring_rgb;
    const uint16_t bright = zv_rgb888_to_rgb565_scaled(rgb888, 900u);
    const uint16_t dim = zv_rgb888_to_rgb565_scaled(rgb888, 420u);
    zv_status_t err;

    switch (state) {
    case ZV_AVATAR_LISTENING:
        /* Paired pickup bars echo the approved listening halo without extra assets. */
        for (int i = 0; i < 3; i++) {
            const int16_t h = (int16_t)(14 + i * 10);
            err = fill(c, (int16_t)(62 + i * 7), (int16_t)(cy - h / 2),
                       (int16_t)(66 + i * 7), (int16_t)(cy + h / 2), i == 2 ? bright : dim);
            if (err != ZV_OK) return err;
            err = fill(c, (int16_t)(294 - i * 7), (int16_t)(cy - h / 2),
                       (int16_t)(298 - i * 7), (int16_t)(cy + h / 2), i == 2 ? bright : dim);
            if (err != ZV_OK) return err;
        }
        break;
    case ZV_AVATAR_THINKING:
        /* Small orbit dots, intentionally cheap so the face stays dominant. */
        for (int i = -1; i <= 1; i++) {
            err = fill(c, (int16_t)(cx + i * 18 - 3), 270, (int16_t)(cx + i * 18 + 3), 276,
                       i + 1 == (int)(face->ring_sweep_deg / 120u) ? bright : dim);
            if (err != ZV_OK) return err;
        }
        err = fill(c, 106, 104, 111, 109, bright);
        if (err != ZV_OK) return err;
        err = fill(c, 249, 122, 254, 127, bright);
        if (err != ZV_OK) return err;
        break;
    case ZV_AVATAR_SPEAKING:
        /* Voice-energy bars at the ear line complement the animated mouth/waveform. */
        for (int i = 0; i < 3; i++) {
            const int16_t h = (int16_t)(18 + i * 12);
            err = fill(c, (int16_t)(52 + i * 8), (int16_t)(cy - h / 2),
                       (int16_t)(56 + i * 8), (int16_t)(cy + h / 2), bright);
            if (err != ZV_OK) return err;
            err = fill(c, (int16_t)(304 - i * 8), (int16_t)(cy - h / 2),
                       (int16_t)(308 - i * 8), (int16_t)(cy + h / 2), bright);
            if (err != ZV_OK) return err;
        }
        break;
    case ZV_AVATAR_EXECUTING:
        err = fill(c, 112, 276, 248, 282, dim);
        if (err != ZV_OK) return err;
        err = fill(c, 112, 276, 206, 282, bright);
        if (err != ZV_OK) return err;
        break;
    case ZV_AVATAR_SUCCESS:
        /* Pixel check badge. */
        err = fill(c, 166, 269, 174, 277, bright);
        if (err != ZV_OK) return err;
        err = fill(c, 172, 275, 180, 283, bright);
        if (err != ZV_OK) return err;
        err = fill(c, 178, 269, 186, 277, bright);
        if (err != ZV_OK) return err;
        err = fill(c, 184, 263, 192, 271, bright);
        if (err != ZV_OK) return err;
        break;
    case ZV_AVATAR_ERROR:
        /* Compact warning marker. */
        err = fill(c, 171, 263, 189, 267, bright);
        if (err != ZV_OK) return err;
        err = fill(c, 166, 267, 194, 271, bright);
        if (err != ZV_OK) return err;
        err = fill(c, 161, 271, 199, 275, bright);
        if (err != ZV_OK) return err;
        err = fill(c, 177, 267, 183, 273, zv_rgb888_to_rgb565(ZV_COLOR_BACKGROUND));
        if (err != ZV_OK) return err;
        break;
    case ZV_AVATAR_APPROVAL:
    case ZV_AVATAR_IDLE:
    default:
        break;
    }

    /* Link presentation reuses avatar states but gets its own small semantic cue. */
    if (chrome != NULL && chrome->status_text != NULL) {
        if (status_is(chrome->status_text, "CONNECTING")) {
            for (int i = -1; i <= 1; i++) {
                err = fill(c, (int16_t)(cx + i * 18 - 3), 275, (int16_t)(cx + i * 18 + 3), 281,
                           i == 0 ? bright : dim);
                if (err != ZV_OK) return err;
            }
        } else if (status_is(chrome->status_text, "OFFLINE") ||
                   status_is(chrome->status_text, "DEGRADED")) {
            err = fill(c, 168, 271, 192, 275, bright);
            if (err != ZV_OK) return err;
        } else if (status_is(chrome->status_text, "STARTING")) {
            /* Zenti waking: three dots rising below the head, quiet and cheap. */
            static const int16_t k_wake[3][4] = {
                {-18, 282, -12, 288}, {-2, 272, 4, 278}, {14, 262, 20, 268},
            };
            for (int i = 0; i < 3; i++) {
                err = fill(c, (int16_t)(cx + k_wake[i][0]), k_wake[i][1],
                           (int16_t)(cx + k_wake[i][2]), k_wake[i][3], i == 2 ? bright : dim);
                if (err != ZV_OK) return err;
            }
        }
    }
    return ZV_OK;
}

static zv_status_t paint_approval_panel(zv_ui_canvas_t *c, uint32_t rgb888)
{
    const uint16_t panel = zv_rgb888_to_rgb565(ZV_COLOR_NAVY);
    const uint16_t approve = zv_rgb888_to_rgb565(ZV_COLOR_SUCCESS);
    const uint16_t deny = zv_rgb888_to_rgb565(ZV_COLOR_ERROR);
    const uint16_t ink = zv_rgb888_to_rgb565(0xD9F2FFu);
    zv_status_t err = fill(c, 48, 240, 312, 320, panel);
    if (err != ZV_OK) return err;

    err = fill(c, ZV_UI_APPROVE_X0, ZV_UI_APPROVE_Y0, ZV_UI_APPROVE_X1, ZV_UI_APPROVE_Y1, approve);
    if (err != ZV_OK) return err;
    err = fill(c, ZV_UI_DENY_X0, ZV_UI_DENY_Y0, ZV_UI_DENY_X1, ZV_UI_DENY_Y1, deny);
    if (err != ZV_OK) return err;

    err = zv_ui_paint_text(c, 75, 277, "APPROVE", ink, 2);
    if (err != ZV_OK) return err;
    err = zv_ui_paint_text(c, 219, 277, "DENY", ink, 2);
    if (err != ZV_OK) return err;

    /* Thin state accent ties the panel back to the approval ring. */
    return fill(c, 48, 240, 312, 244, zv_rgb888_to_rgb565_scaled(rgb888, 900u));
}

zv_status_t zv_ui_paint_zenti_head(zv_ui_canvas_t *canvas, int16_t cx, int16_t cy, uint8_t scale,
                                   uint32_t tint_rgb888)
{
    if (canvas == NULL || canvas->fill_rect == NULL || scale == 0u) {
        return ZV_ERR_INVALID_ARG;
    }
    const int16_t s = (int16_t)scale;
    const int16_t origin_x = (int16_t)(cx - (ZV_ZENTI_SPRITE_W * s) / 2);
    const int16_t origin_y = (int16_t)(cy - (ZV_ZENTI_SPRITE_H * s) / 2);
    const bool tint = tint_rgb888 != 0u;
    for (uint16_t i = 0; i < ZV_ZENTI_SPRITE_RUNS; i++) {
        const uint8_t *rec = &zv_zenti_runs[(size_t)i * 4u];
        const uint8_t y = rec[0], x0 = rec[1], len = rec[2], idx = rec[3];
        if (idx == 0u || idx >= ZV_ZENTI_PALETTE_COUNT || len == 0u) {
            continue;
        }
        uint16_t color = zv_zenti_palette[idx];
        /*
         * Keep Zenti's helmet, visor and eyes in canonical mascot colors.
         * Earlier builds tinted the large white helmet with the state-ring hue,
         * which collapsed contrast on the physical 360x360 panel and made the
         * mascot read as a muddy green/orange blob. State emotion belongs in
         * the ring and surrounding effects, not in the mascot's base colors.
         */
        (void)tint;
        const int16_t px0 = (int16_t)(origin_x + (int16_t)x0 * s);
        const int16_t py0 = (int16_t)(origin_y + (int16_t)y * s);
        zv_status_t err = canvas->fill_rect(canvas->ctx, px0, py0, (int16_t)(px0 + (int16_t)len * s),
                                            (int16_t)(py0 + s), color);
        if (err != ZV_OK) {
            return err;
        }
    }
    return ZV_OK;
}



extern const unsigned char zv_reference_blob[];
extern const unsigned int zv_reference_blob_size;

/* Four subpixel samples soften the small moving mouth on the 360px panel.
 * Blend against the actual portrait pixel instead of a flat rectangular mask. */
static void mouth_row(uint16_t *row, uint16_t count, int16_t x0, int16_t y,
                      int16_t cy, int16_t rx, int16_t ry, uint16_t color)
{
    if (y < cy-ry || y > cy+ry) return;
    for (uint16_t i=0; i<count; ++i) {
        int32_t x=(int32_t)x0+i-180;
        if (x < -rx || x > rx) continue;
        unsigned coverage=0;
        for (int sy=-1; sy<=1; sy+=2) for (int sx=-1; sx<=1; sx+=2) {
            int32_t dx=x*4+sx, dy=((int32_t)y-cy)*4+sy;
            if (dx*dx*ry*ry + dy*dy*rx*rx <= 16*rx*rx*ry*ry) ++coverage;
        }
        uint16_t base=row[i];
        unsigned r=(((base>>11)&31u)*(4u-coverage)+((color>>11)&31u)*coverage+2u)/4u;
        unsigned g=(((base>>5)&63u)*(4u-coverage)+((color>>5)&63u)*coverage+2u)/4u;
        unsigned b=((base&31u)*(4u-coverage)+(color&31u)*coverage+2u)/4u;
        row[i]=(uint16_t)((r<<11)|(g<<5)|b);
    }
}

/* Flash-resident artwork, one bounded stack row, zero core heap allocation. */
static zv_status_t paint_reference_head(zv_ui_canvas_t *c, const zv_face_frame_t *face)
{
    if (zv_reference_blob_size < 8u || memcmp(zv_reference_blob, "ZR4\0", 4u) != 0) return ZV_ERR_INVALID_ARG;
    const uint16_t width = (uint16_t)(zv_reference_blob[4] | ((uint16_t)zv_reference_blob[5] << 8));
    const uint16_t height = (uint16_t)(zv_reference_blob[6] | ((uint16_t)zv_reference_blob[7] << 8));
    if (width != 204u || height != 188u) return ZV_ERR_INVALID_ARG;
    uint16_t row[204];
    size_t cursor = 8u;
    for (uint16_t y = 0u; y < height; y++) {
        if (cursor + 2u > zv_reference_blob_size) return ZV_ERR_INVALID_ARG;
        const uint16_t left = zv_reference_blob[cursor++];
        const uint16_t length = zv_reference_blob[cursor++];
        if (left + length > width || cursor + (size_t)length * 2u > zv_reference_blob_size) return ZV_ERR_INVALID_ARG;
        for (uint16_t x = 0u; x < length; x++) {
            row[x] = (uint16_t)(zv_reference_blob[cursor] | ((uint16_t)zv_reference_blob[cursor + 1u] << 8));
            cursor += 2u;
        }
        if (length == 0u) continue;
        if (face->mouth_open > 0u) {
            uint16_t opening=face->mouth_open > 1000u ? 1000u : face->mouth_open;
            int16_t aperture=(int16_t)(1u+opening*6u/1000u);
            int16_t x0=(int16_t)(78+left), py=(int16_t)(88+y);
            /* The artwork's smile is centred at y=237. The old y=241
             * overlay added a second mouth below it instead of opening it. */
            mouth_row(row,length,x0,py,237,10,aperture,zv_rgb888_to_rgb565(0x010A1Bu));
        }
        zv_status_t err;
        if (c->draw_row != NULL) {
            err = c->draw_row(c->ctx, (int16_t)(78 + left), (int16_t)(88 + y), length, row);
            if (err != ZV_OK) return err;
        } else {
            /* Generic canvas fallback is correct; hardware always supplies draw_row. */
            for (uint16_t x = 0u; x < length;) {
                uint16_t next = (uint16_t)(x + 1u);
                while (next < length && row[next] == row[x]) next++;
                err = fill(c, (int16_t)(78 + left + x), (int16_t)(88 + y),
                           (int16_t)(78 + left + next), (int16_t)(89 + y), row[x]);
                if (err != ZV_OK) return err;
                x = next;
            }
        }
    }
    if (cursor != zv_reference_blob_size) return ZV_ERR_INVALID_ARG;
    return ZV_OK;
}

static zv_status_t paint_clock(zv_ui_canvas_t *c, const char *text, uint16_t ink)
{
    if (text == NULL || text[0] == '\0') return ZV_OK;
    const char *suffix = strchr(text, ' ');
    /* Unsynchronized time stays readable and never becomes a fabricated clock. */
    if (strchr(text, ':') == NULL || strlen(text) > 10u) {
        return zv_ui_paint_text(c, (int16_t)(180 - zv_ui_text_width(text, 1u) / 2), 53, text, ink, 1u);
    }
    char digits[6];
    size_t len = suffix != NULL ? (size_t)(suffix - text) : strlen(text);
    if (len >= sizeof(digits)) return ZV_ERR_INVALID_ARG;
    memcpy(digits, text, len); digits[len] = '\0';
    const int16_t width = (int16_t)(zv_ui_text_width(digits, 4u) + (suffix != NULL ? 26 : 0));
    const int16_t x = (int16_t)(180 - width / 2);
    zv_status_t err = zv_ui_paint_text(c, x, 47, digits, ink, 4u);
    if (err != ZV_OK || suffix == NULL) return err;
    return zv_ui_paint_text(c, (int16_t)(x + zv_ui_text_width(digits, 4u) + 2), 62, suffix + 1, ink, 2u);
}

static uint16_t nous_blend(uint16_t a, uint16_t b, unsigned weight)
{
    unsigned inverse=1000u-weight;
    unsigned r=(((a>>11)&31u)*inverse+((b>>11)&31u)*weight+500u)/1000u;
    unsigned g=(((a>>5)&63u)*inverse+((b>>5)&63u)*weight+500u)/1000u;
    unsigned blue=((a&31u)*inverse+(b&31u)*weight+500u)/1000u;
    return (uint16_t)((r<<11)|(g<<5)|blue);
}

/* Preserve the portrait's upper lip and move its actual lower lip downward.
 * A small inverse warp fades to zero at the mouth corners and below the lip;
 * no expression-sheet rectangle or ellipse is pasted over her lipstick. */
static void nous_mouth_row(uint16_t *row, int16_t y, uint16_t opening,
                           const uint16_t patch[27][33])
{
    if (!opening || y<217 || y>=234) return;
    for (int16_t dx=-13; dx<=13; ++dx) {
        int32_t weight=196-dx*dx;
        int32_t aperture=(int32_t)opening*5*weight/196;
        if (!aperture) continue;
        int32_t seam=217000+2000*weight/196;
        int32_t bottom=seam+aperture;
        int32_t py=(int32_t)y*1000;
        if (py+500<=seam) continue;
        int32_t source=seam;
        if (py>bottom) source=seam+(py-bottom)*(234000-seam)/(234000-bottom);
        int32_t sy=(source-208000)/1000;
        unsigned fraction=(unsigned)((source-208000)%1000);
        if (sy<0 || sy>=26) continue;
        unsigned px=(unsigned)(180+dx-164);
        uint16_t lip=nous_blend(patch[sy][px],patch[sy+1][px],fraction);
        int32_t top_edge=py-500>seam ? py-500:seam;
        int32_t lower_edge=py+500<bottom ? py+500:bottom;
        unsigned coverage=lower_edge>top_edge ? (unsigned)(lower_edge-top_edge):0u;
        uint16_t cavity=zv_rgb888_to_rgb565(0x35151Du);
        /* A restrained upper-teeth highlight stays inside the opening. */
        if (opening>=500u && dx>=-8 && dx<=8 && py>seam && py<seam+1300)
            cavity=zv_rgb888_to_rgb565(0xEBCDC1u);
        row[180+dx-68]=nous_blend(lip,cavity,coverage);
    }
}

static zv_status_t paint_nous(zv_ui_canvas_t *canvas, const zv_face_frame_t *face, zv_avatar_state_t state)
{
    /* Speaking uses one stable base portrait. Switching whole expression
     * sheets at an amplitude threshold, inside only a mouth-sized crop,
     * pasted a rectangular piece of a different face onto the current one. */
    bool speaking=state==ZV_AVATAR_SPEAKING;
    if (speaking) state=ZV_AVATAR_IDLE;
    const nous_run_t *nous_runs=nous_frames[state].runs;
    size_t run_count=nous_frames[state].count;
    /* Check immutable flash data once per frame, including color corruption. */
    static uint8_t validated[ZV_AVATAR_STATE_COUNT];
    if (validated[state]==0u) {
        uint32_t hash=2166136261u; unsigned expected_y=0u, expected_x=0u;
        bool valid=true;
        for (size_t i=0; i<run_count; ++i) {
            const nous_run_t *run=&nous_runs[i];
            if (run->y!=expected_y || run->x!=expected_x || !run->count || expected_x+run->count>224u) valid=false;
            expected_x+=run->count;
            if (expected_x==224u) { expected_x=0u; ++expected_y; }
            const uint8_t bytes[5]={run->y,run->x,run->count,(uint8_t)run->color,(uint8_t)(run->color>>8)};
            for (size_t j=0;j<5u;++j) hash=(hash^bytes[j])*16777619u;
        }
        validated[state]=(uint8_t)(valid && expected_y==224u && expected_x==0u && hash==nous_frames[state].hash ? 1u : 2u);
    }
    if (validated[state]!=1u) return paint_reference_head(canvas,face);
    /* Cache only the idle portrait's mouth neighborhood: 1782 static bytes,
     * populated once from the validated immutable artwork, no core heap. */
    static uint16_t mouth_patch[27][33];
    static bool mouth_cached;
    if (speaking && !mouth_cached) {
        for (size_t i=0; i<run_count; ++i) {
            const nous_run_t *run=&nous_runs[i];
            if (run->y<126u || run->y>=153u) continue;
            unsigned left=run->x>96u ? run->x:96u;
            unsigned right=(unsigned)run->x+run->count;
            if (right>129u) right=129u;
            for (unsigned x=left; x<right; ++x) mouth_patch[run->y-126u][x-96u]=run->color;
        }
        mouth_cached=true;
    }
    uint16_t row[224]; size_t index=0;
    for (int16_t y=0; y<224; ++y) {
        memset(row, 0, sizeof(row));
        while (index < run_count && nous_runs[index].y == (uint8_t)y) {
            for (unsigned x=nous_runs[index].x; x<(unsigned)nous_runs[index].x+nous_runs[index].count; ++x) row[x]=nous_runs[index].color;
            ++index;
        }
        if (speaking && face->mouth_open > 0u) {
            uint16_t opening=face->mouth_open > 1000u ? 1000u : face->mouth_open;
            nous_mouth_row(row,(int16_t)(82+y),opening,mouth_patch);
        }
        zv_status_t result=ZV_OK;
        if (canvas->draw_row) result=canvas->draw_row(canvas->ctx,68,(int16_t)(82+y),224,row);
        else for (int16_t x=0; x<224 && result==ZV_OK; ++x) result=fill(canvas,(int16_t)(68+x),(int16_t)(82+y),(int16_t)(69+x),(int16_t)(83+y),row[x]);
        if (result != ZV_OK) return result;
    }
    /* Mouth and eyelid poses follow the same real state/envelope as Zenti. */
    if (face->eye_open < 200u) {
        zv_status_t result=fill(canvas,143,173,163,181,zv_rgb888_to_rgb565(0xE9AC8Du));
        if (result != ZV_OK) return result;
        result=fill(canvas,192,173,212,181,zv_rgb888_to_rgb565(0xE9AC8Du));
        if (result != ZV_OK) return result;
    }
    return ZV_OK;
}

zv_status_t zv_ui_paint_frame(zv_ui_canvas_t *canvas, const zv_face_frame_t *face, const zv_avatar_state_t state,
                              const zv_ui_chrome_t *chrome)
{
    if (canvas == NULL || face == NULL || (unsigned)state >= (unsigned)ZV_AVATAR_STATE_COUNT) {
        return ZV_ERR_INVALID_ARG;
    }

    const int16_t cx = (int16_t)(ZV_UI_W / 2);
    const int16_t cy = 192;
    zv_status_t err;

    err = fill(canvas, 0, 0, (int16_t)ZV_UI_W, (int16_t)ZV_UI_H, zv_rgb888_to_rgb565(ZV_COLOR_BACKGROUND));
    if (err != ZV_OK) {
        return err;
    }

    err = (chrome != NULL && chrome->use_zenti_head) ? ZV_OK : paint_orbital_field(canvas, cx, cy, face->ring_rgb);
    if (err != ZV_OK) {
        return err;
    }

    err = (chrome != NULL && chrome->use_zenti_head)
        ? paint_soft_ring(canvas, cx, cy, 118, face->ring_rgb, face->ring_intensity < 850u ? 850u : face->ring_intensity, face->ring_sweep, face->ring_sweep_deg)
        : paint_soft_ring_legacy(canvas, cx, cy, 118, face->ring_rgb, face->ring_intensity, face->ring_sweep, face->ring_sweep_deg);
    if (err != ZV_OK) {
        return err;
    }

    const bool zenti = chrome != NULL && chrome->use_zenti_head;
    if (zenti) {
        err = s_nous ? paint_nous(canvas, face, state) : paint_reference_head(canvas, face);
        if (err != ZV_OK) return err;
    } else {
    err = paint_core(canvas, cx, cy, face->ring_rgb);
    if (err != ZV_OK) {
        return err;
    }

    /* Eyes: left/right ovals; eye_open scales height; gaze shifts centres. */
    const int16_t eye_y = (int16_t)(cy - 28 + (int16_t)((int32_t)face->gaze_y * 12 / 1000));
    const int16_t eye_dx = 42;
    const int16_t gaze_shift = (int16_t)((int32_t)face->gaze_x * 14 / 1000);
    int16_t eye_ry = (int16_t)(2 + (int32_t)face->eye_open * 8 / 1000);
    if (face->eye_open < 80u) {
        eye_ry = 1;
    }
    uint16_t ink = zv_rgb888_to_rgb565(0xD9F2FFu);
    err = paint_ellipse(canvas, (int16_t)(cx - eye_dx + gaze_shift), eye_y, 16, eye_ry, ink);
    if (err != ZV_OK) {
        return err;
    }
    err = paint_ellipse(canvas, (int16_t)(cx + eye_dx + gaze_shift), eye_y, 16, eye_ry, ink);
    if (err != ZV_OK) {
        return err;
    }

    /* Mouth: an open voice aperture, or a cut-out expression arc at rest. */
    int16_t mouth_w = (int16_t)(8 + (int32_t)face->mouth_width * 28 / 1000);
    int16_t mouth_h = (int16_t)(2 + (int32_t)face->mouth_open * 22 / 1000);
    int16_t mouth_curve = face->mouth_curve;
    int16_t mouth_y = (int16_t)(cy + 40 + (int16_t)(mouth_curve > 0 ? (int16_t)(mouth_curve / 80) : 0));
    err = paint_ellipse(canvas, cx, mouth_y, mouth_w, mouth_h, ink);
    if (err != ZV_OK) {
        return err;
    }
    if (face->mouth_open < 300u && face->mouth_curve != 0) {
        uint16_t core = zv_rgb888_to_rgb565_scaled(face->ring_rgb, 55u);
        int16_t cut_y = face->mouth_curve > 0 ? (int16_t)(mouth_y - mouth_h) : mouth_y;
        err = fill(canvas, (int16_t)(cx - mouth_w), cut_y, (int16_t)(cx + mouth_w),
                   (int16_t)(cut_y + mouth_h + 1), core);
        if (err != ZV_OK) {
            return err;
        }
    }
    } /* end procedural face */

    uint16_t ink = zv_rgb888_to_rgb565(0xD9F2FFu);
    uint16_t dim_ink = zv_rgb888_to_rgb565_scaled(0xD9F2FFu, 720u);
    if (chrome != NULL && chrome->date_text != NULL && chrome->date_text[0] != '\0') {
        int16_t dw = zv_ui_text_width(chrome->date_text, 2);
        err = zv_ui_paint_text(canvas, (int16_t)((ZV_UI_W - dw) / 2), 27, chrome->date_text, ink, 2);
        if (err != ZV_OK) {
            return err;
        }
    }
    if (chrome != NULL && chrome->brand_text != NULL && chrome->brand_text[0] != '\0') {
        err = zv_ui_paint_text(canvas, 12, 51, chrome->brand_text, dim_ink, 1);
        if (err != ZV_OK) {
            return err;
        }
    }
    err = paint_connectivity(canvas, chrome, face->ring_rgb);
    if (err != ZV_OK) {
        return err;
    }
    err = paint_battery(canvas, chrome, face->ring_rgb);
    if (err != ZV_OK) {
        return err;
    }
    if (chrome != NULL && chrome->clock_text != NULL && chrome->clock_text[0] != '\0') {
        err = paint_clock(canvas, chrome->clock_text, ink);
        if (err != ZV_OK) {
            return err;
        }
    }
    err = paint_state_fx(canvas, state, chrome, cx, cy, face);
    if (err != ZV_OK) {
        return err;
    }

    if (chrome != NULL) {
        err = paint_wave(canvas, chrome, zv_rgb888_to_rgb565_scaled(face->ring_rgb, 900),
                         zv_rgb888_to_rgb565_scaled(face->ring_rgb, 280));
        if (err != ZV_OK) {
            return err;
        }
        if (chrome->notice != NULL && chrome->notice[0] != '\0' && state != ZV_AVATAR_APPROVAL) {
            char notice[35];
            size_t len = strlen(chrome->notice);
            if (len >= sizeof(notice)) len = sizeof(notice) - 1u;
            memcpy(notice, chrome->notice, len); notice[len] = '\0';
            err = paint_chip(canvas, 66, 250, 294, 278, face->ring_rgb, 700u, 100u);
            if (err != ZV_OK) return err;
            err = zv_ui_paint_text(canvas, (int16_t)(180 - zv_ui_text_width(notice, 1u) / 2), 260, notice, ink, 1u);
            if (err != ZV_OK) return err;
        }
    }

    if (state == ZV_AVATAR_APPROVAL && (chrome == NULL || !chrome->menu_open)) {
        err = paint_approval_panel(canvas, face->ring_rgb);
        if (err != ZV_OK) return err;
        if (chrome != NULL && chrome->notice != NULL) {
            char notice[35];
            size_t len = strlen(chrome->notice);
            /* Never silently shorten the action being approved. */
            if (len >= sizeof(notice)) return ZV_ERR_INVALID_ARG;
            memcpy(notice, chrome->notice, len); notice[len] = '\0';
            err = zv_ui_paint_text(canvas, (int16_t)(180 - zv_ui_text_width(notice, 1u) / 2), 249, notice, ink, 1u);
            if (err != ZV_OK) return err;
        }
        err = paint_mic(canvas, chrome, face->ring_rgb);
        if (err != ZV_OK) return err;
        return paint_home(canvas, chrome, face->ring_rgb);
    }

    const char *status = (chrome != NULL && chrome->status_text != NULL) ? chrome->status_text : zv_avatar_state_name(state);
    if (chrome != NULL && chrome->gateway_online && state == ZV_AVATAR_IDLE &&
        (status_is(status, "IDLE") || status_is(status, "ONLINE"))) status = "ONLINE";
    int16_t sw = zv_ui_text_width(status, 2u);
    int16_t chip_w = (int16_t)(sw + 44);
    if (chip_w < 154) chip_w = 154;
    if (chip_w > 202) chip_w = 202;
    int16_t chip_x = (int16_t)(180 - chip_w / 2);
    /* Text metrics include a final 2px gap; center the visible glyphs. */
    int16_t status_x = (int16_t)(180 - (sw > 0 ? sw - 2 : 0) / 2);
    err = paint_chip(canvas, chip_x, 284, (int16_t)(chip_x + chip_w), 315, ZV_COLOR_ZENTARI_BLUE, 1000u, 160u);
    if (err != ZV_OK) return err;
    uint32_t dot_rgb = chrome != NULL && chrome->gateway_online ? 0x24F6A4u : face->ring_rgb;
    err = paint_ellipse(canvas, (int16_t)(status_x - 14), 300, 6, 6, zv_rgb888_to_rgb565(dot_rgb));
    if (err != ZV_OK) return err;
    err = zv_ui_paint_text(canvas, status_x, 293, status, ink, 2u);
    if (err != ZV_OK) return err;

    if (chrome != NULL && chrome->menu_open) {
        /* Full appliance menu: large rows, quiet chrome, no tiny dashboard controls. */
        err = paint_round_rect(canvas, 48, 84, 312, 300, 16,
                               zv_rgb888_to_rgb565(ZV_COLOR_NAVY));
        if (err != ZV_OK) return err;
        err = zv_ui_paint_text(canvas, 68, 90, "MENU", ink, 2);
        if (err != ZV_OK) return err;
        size_t items = chrome->menu_count;
        if (items > (size_t)ZV_UI_MAX_MENU) items = (size_t)ZV_UI_MAX_MENU;
        for (size_t i = 0; i < items; i++) {
            if (chrome->menu_items[i] == NULL) continue;
            int16_t row_y = (int16_t)(ZV_UI_MENU_Y0 + (int16_t)i * ZV_UI_MENU_ITEM_H);
            uint16_t row_color = zv_rgb888_to_rgb565_scaled(face->ring_rgb,
                chrome->screen == ZV_UI_SCREEN_MENU && chrome->menu_focus == (uint8_t)i ? 380u : 170u);
            err = paint_round_rect(canvas, ZV_UI_MENU_X0, row_y, ZV_UI_MENU_X1,
                                   (int16_t)(row_y + 33), 8,
                                   row_color);
            if (err != ZV_OK) return err;
            err = fill(canvas, 74, (int16_t)(row_y + 7), 78, (int16_t)(row_y + 25),
                       zv_rgb888_to_rgb565_scaled(face->ring_rgb, 760u));
            if (err != ZV_OK) return err;
            err = zv_ui_paint_text(canvas, 90, (int16_t)(row_y + 8), chrome->menu_items[i], ink, 2);
            if (err != ZV_OK) return err;
            /* Right chevron. */
            err = fill(canvas, 272, (int16_t)(row_y + 12), 276, (int16_t)(row_y + 16), dim_ink);
            if (err != ZV_OK) return err;
            err = fill(canvas, 276, (int16_t)(row_y + 16), 280, (int16_t)(row_y + 20), dim_ink);
            if (err != ZV_OK) return err;
            err = fill(canvas, 272, (int16_t)(row_y + 20), 276, (int16_t)(row_y + 24), dim_ink);
            if (err != ZV_OK) return err;
        }
    }

    if (chrome != NULL && chrome->screen == ZV_UI_SCREEN_NETWORK) {
        return paint_network_screen(canvas, chrome, face->ring_rgb);
    }
    if (chrome != NULL && chrome->screen == ZV_UI_SCREEN_VOLUME) {
        return paint_volume_screen(canvas, chrome, face->ring_rgb);
    }
    if (chrome != NULL && chrome->screen == ZV_UI_SCREEN_CONFIRM) {
        return paint_confirm_screen(canvas, face->ring_rgb);
    }
    err = paint_volume_indicator(canvas, chrome, face->ring_rgb);
    if (err != ZV_OK) return err;
    err = paint_cancel(canvas, chrome);
    if (err != ZV_OK) return err;
    err = paint_mic(canvas, chrome, face->ring_rgb);
    if (err != ZV_OK) return err;
    return paint_home(canvas, chrome, face->ring_rgb);
}
