#!/usr/bin/env python3
"""Generate the 8-bit Zenti head sprite as RGB565 RLE C data.

Draws a compact pixel-art head matching the Zenti mark (rounded white helmet,
blue visor with large eyes, side ear cups) at 1× pixel scale, then emits:

  components/zv_core/include/zv_zenti_sprite.h
  components/zv_core/src/zv_zenti_sprite.c

Palette is fixed and tiny (8 colours) so the device stays allocation-free and
needs no PNG decoder. Re-run after any art change; the header records size and
a content hash for the manifest.

    python tools/gen_zenti_sprite.py
"""
from __future__ import annotations

import hashlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT_H = ROOT / "components/zv_core/include/zv_zenti_sprite.h"
OUT_C = ROOT / "components/zv_core/src/zv_zenti_sprite.c"

# Fixed 8-colour palette (RGB888) chosen to match the Zenti mark.
PALETTE = [
    0x000000,  # 0 transparent/void (skipped in runs)
    0x061426,  # 1 near-black navy outline
    0xFFFFFF,  # 2 helmet white
    0xD8EEFF,  # 3 cool helmet shade
    0x2D6FC3,  # 4 mid blue
    0x1F75FE,  # 5 Zentari blue
    0x8CE7FF,  # 6 bright cyan eye highlight
    0x061A3A,  # 7 deep visor navy
]

W, H = 42, 42


def px(grid: list[list[int]], x: int, y: int, c: int) -> None:
    if 0 <= y < len(grid) and 0 <= x < len(grid[y]):
        grid[y][x] = c


def rect(grid: list[list[int]], x0: int, y0: int, x1: int, y1: int, c: int) -> None:
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            px(grid, x, y, c)


def disk(grid: list[list[int]], cx: int, cy: int, r: int, c: int) -> None:
    for y in range(cy - r, cy + r + 1):
        for x in range(cx - r, cx + r + 1):
            if (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r:
                px(grid, x, y, c)


def draw_head() -> list[list[int]]:
    """Canonical pixel-art head with clear visor, friendly eyes, and headset cups."""
    source_w = source_h = 64
    grid = [[0] * source_w for _ in range(source_h)]
    rect(grid, 3, 25, 12, 43, 1)
    rect(grid, 5, 27, 11, 41, 4)
    rect(grid, 7, 29, 11, 39, 5)
    rect(grid, 8, 31, 10, 37, 6)
    rect(grid, 51, 25, 60, 43, 1)
    rect(grid, 52, 27, 58, 41, 4)
    rect(grid, 52, 29, 56, 39, 5)
    rect(grid, 53, 31, 55, 37, 6)

    # Helmet shell: dark keyline, cool lower shade, and restrained highlights.
    disk(grid, 32, 33, 26, 1)
    disk(grid, 32, 33, 24, 2)
    rect(grid, 11, 36, 53, 48, 3)
    rect(grid, 16, 17, 20, 18, 3)
    rect(grid, 13, 21, 15, 23, 3)
    rect(grid, 44, 17, 47, 18, 3)
    rect(grid, 5, 29, 7, 35, 6)
    rect(grid, 57, 29, 59, 35, 6)

    # Blue forehead crest and reflective edge.
    rect(grid, 21, 9, 42, 15, 5)
    rect(grid, 24, 10, 39, 12, 6)
    rect(grid, 18, 14, 45, 17, 4)
    rect(grid, 29, 15, 35, 16, 6)

    # Deep visor keeps the face readable against the white helmet.
    disk(grid, 32, 35, 20, 1)
    disk(grid, 32, 35, 18, 7)
    rect(grid, 15, 29, 49, 40, 7)
    rect(grid, 27, 20, 36, 21, 4)
    rect(grid, 18, 23, 27, 24, 4)
    rect(grid, 37, 23, 46, 24, 4)
    rect(grid, 15, 32, 16, 36, 6)
    rect(grid, 48, 32, 49, 36, 6)

    # Cyan eyes, navy pupils, and two crisp catchlights.
    for cx in (24, 40):
        disk(grid, cx, 33, 8, 1)
        disk(grid, cx, 33, 7, 6)
        disk(grid, cx, 35, 4, 5)
        disk(grid, cx, 36, 2, 7)
        rect(grid, cx - 3, 27, cx, 29, 2)
        px(grid, cx + 3, 30, 2)
        px(grid, cx - 3, 32, 2)

    # Friendly smile and a subtle chin glint.
    rect(grid, 27, 46, 36, 47, 6)
    rect(grid, 29, 48, 34, 49, 6)
    px(grid, 26, 45, 6)
    px(grid, 37, 45, 6)
    rect(grid, 29, 51, 34, 51, 3)

    # Antenna and pixel heart remain the recognizable Zenti mark.
    rect(grid, 30, 3, 33, 8, 1)
    rect(grid, 31, 4, 32, 8, 5)
    rect(grid, 27, 1, 30, 3, 5)
    rect(grid, 33, 1, 36, 3, 5)
    rect(grid, 28, 2, 35, 5, 5)
    rect(grid, 30, 5, 33, 6, 5)

    return [
        [grid[min(source_h - 1, y * source_h // H)][min(source_w - 1, x * source_w // W)]
         for x in range(W)]
        for y in range(H)
    ]


def rgb565(r: int, g: int, b: int) -> int:
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def to_runs(grid: list[list[int]]) -> list[tuple[int, int, int]]:
    """Horizontal RLE: (y, x_start, run_length, colour) merged per row."""
    runs: list[tuple[int, int, int, int]] = []
    for y, row in enumerate(grid):
        x = 0
        while x < W:
            c = row[x]
            if c == 0:
                x += 1
                continue
            x0 = x
            while x < W and row[x] == c:
                x += 1
            runs.append((y, x0, x - x0, c))
    return runs


def emit(grid: list[list[int]]) -> None:
    runs = to_runs(grid)
    colors = [rgb565((p >> 16) & 0xFF, (p >> 8) & 0xFF, p & 0xFF) for p in PALETTE]
    digest = hashlib.sha256(bytes(grid[y][x] for y in range(H) for x in range(W))).hexdigest()[:16]

    # Pack runs as u16: [y:6][x:6][len:4] is tight but opaque; use flat triples
    # of u8 y,x,len + u8 colour for clarity and easy host tests.
    blob = bytearray()
    for y, x0, ln, c in runs:
        blob += bytes((y & 0xFF, x0 & 0xFF, ln & 0xFF, c & 0xFF))

    OUT_H.write_text(
        f"""/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 *
 * Generated by tools/gen_zenti_sprite.py — 8-bit Zenti head, RGB565 RLE.
 * hash={digest} size={W}x{H} runs={len(runs)}
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {{
#endif

#define ZV_ZENTI_SPRITE_W {W}
#define ZV_ZENTI_SPRITE_H {H}
#define ZV_ZENTI_SPRITE_RUNS {len(runs)}
#define ZV_ZENTI_PALETTE_COUNT {len(colors)}
#define ZV_ZENTI_SPRITE_HASH "{digest}"

/** Palette entries as RGB565. Index 0 is transparent and never emitted. */
extern const uint16_t zv_zenti_palette[ZV_ZENTI_PALETTE_COUNT];
/** RLE records: y, x, length, colour_index (colour 0 is skipped). */
extern const uint8_t zv_zenti_runs[ZV_ZENTI_SPRITE_RUNS * 4u];

#ifdef __cplusplus
}}
#endif
""",
        encoding="utf-8",
    )

    palette_lines = ",\n    ".join(f"0x{c:04X}u" for c in colors)
    run_bytes = ", ".join(f"0x{b:02X}" for b in blob)
    OUT_C.write_text(
        f"""/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 *
 * Generated by tools/gen_zenti_sprite.py — do not edit by hand.
 */
#include "zv_zenti_sprite.h"

const uint16_t zv_zenti_palette[ZV_ZENTI_PALETTE_COUNT] = {{
    {palette_lines}
}};

const uint8_t zv_zenti_runs[ZV_ZENTI_SPRITE_RUNS * 4u] = {{
    {run_bytes}
}};
""",
        encoding="utf-8",
    )
    print(f"wrote {OUT_H.relative_to(ROOT)} and {OUT_C.relative_to(ROOT)}")
    print(f"runs={len(runs)} palette={len(colors)} hash={digest}")


def main() -> int:
    emit(draw_head())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
