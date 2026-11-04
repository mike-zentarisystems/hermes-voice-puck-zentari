#!/usr/bin/env python3
"""Deterministic original geometric avatars; stdlib only, no reference images/fonts."""
import argparse
import hashlib
from pathlib import Path
import struct
import zlib

STATES = ('idle', 'listening', 'thinking', 'speaking', 'working', 'success', 'error', 'approval')


def pixels(size, variant=0, state='idle'):
    accent = (131, 230, 219) if variant == 0 else (246, 191, 114)
    image = []
    for y in range(size):
        row = []
        for x in range(size):
            u, v = (x + .5) / size, (y + .5) / size
            radius = (u - .5)**2 + (v - .5)**2
            color = (16, 21, 34, 0)
            if radius < .225: color = (*accent, 255)
            if radius < .20: color = (25, 40, 56, 255)
            if .24 < u < .76 and .31 < v < .64: color = (9, 19, 30, 255)
            ey = .44 if state != 'thinking' else .41
            if any((u - ex)**2 + (v - ey)**2 < .0036 for ex in (.37, .63)):
                color = (*accent, 255)
            if state == 'error' and .46 < u < .54 and .71 < v < .75: color = (255, 170, 160, 255)
            elif .41 < u < .59 and .71 < v < (.79 if state == 'speaking' else .735): color = (*accent, 255)
            row.append(color)
        image.append(row)
    return image


def png(image):
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    size = len(image)
    raw = b''.join(b'\0' + bytes(c for pixel in row for c in pixel) for row in image)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', size, size, 8, 6, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def rgb(color):
    r, g, b, _ = color
    return ((r & 248) << 8) | ((g & 252) << 3) | (b >> 3)


def generate(root):
    def write(path, data):
        target = root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
    for index, name in enumerate(('zenti', 'orbit')):
        for state in STATES:
            write(f'pwa/client/avatars/{name}-{state}.png', png(pixels(224, index, state)))
        write(f'pwa/client/avatars/{name}.png', png(pixels(224, index)))
    for size in (192, 512):
        write(f'pwa/client/icons/icon-{size}.png', png(pixels(size)))
    # Existing firmware decoder: one bounded RGB565 span per row.
    image = pixels(204)
    blob = bytearray(b'ZR4\0' + struct.pack('<HH', 204, 188))
    for y in range(188):
        row = image[y * 204 // 188]
        occupied = [x for x, color in enumerate(row) if color[3]]
        left = occupied[0] if occupied else 0
        count = occupied[-1] - left + 1 if occupied else 0
        blob.extend((left, count))
        for color in row[left:left+count]: blob.extend(struct.pack('<H', rgb(color)))
    write('assets/avatar/reference-v4.rgb565', bytes(blob))
    lines = ['/* Original procedural Orbit artwork. Apache-2.0. */',
             'typedef struct { uint8_t y,x,count; uint16_t color; } nous_run_t;']
    hashes = []
    for state in STATES:
        runs = []
        digest = 2166136261
        for y, row in enumerate(pixels(224, 1, state)):
            row = [color if color[3] else (16, 21, 34, 255) for color in row]
            x = 0
            while x < 224:
                end = x + 1
                while end < 224 and row[end] == row[x]: end += 1
                color = rgb(row[x])
                runs.append(f'{{{y},{x},{end-x},0x{color:04x}}},')
                for byte in (y, x, end-x, color & 255, color >> 8):
                    digest = ((digest ^ byte) * 16777619) & 0xffffffff
                x = end
        data = '\n'.join(runs)
        hashes.append(digest)
        lines.extend([f'static const nous_run_t nous_{state}[] = {{', data, '};'])
    lines.append('static const struct { const nous_run_t *runs; size_t count; uint32_t hash; } nous_frames[] = {')
    for state, digest in zip(STATES, hashes):
        lines.append(f'{{nous_{state},sizeof(nous_{state})/sizeof(nous_{state}[0]),0x{digest:08x}u}},')
    lines.append('};')
    write('components/zv_core/src/zv_nous_sprite.inc', ('\n'.join(lines) + '\n').encode())


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    generate(parser.parse_args().root)
