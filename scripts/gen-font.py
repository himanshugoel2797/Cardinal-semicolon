#!/usr/bin/env python3
# Copyright (c) 2026 Himanshu Goel
#
# This software is released under the MIT License.
# https://opensource.org/licenses/MIT
#
# Generate lisp/data/font8x16.bin: a classic VGA-style 8x16 bitmap font, 256
# glyphs * 16 bytes (one byte per row, MSB = leftmost pixel), indexable directly
# by character code (glyph N at byte offset N*16). ASCII 32..126 are rasterized
# from DejaVu Sans Mono (a free, embeddable font: Bitstream Vera + DejaVu
# license); all other code points are left blank.
#
# Reproducible: re-run to regenerate the blob. The graphics font loader
# (lisp/lib/font.clp) reads any file in this layout, so the font can be swapped.
#
#   python3 scripts/gen-font.py
#
# Tunables below pick the TTF size / vertical placement that best fits DejaVu
# Sans Mono glyphs into the 8x16 cell; the result is eyeballed via --dump.

import sys
from PIL import Image, ImageFont, ImageDraw

CELL_W, CELL_H = 8, 16
FONT_PATH = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"
FONT_SIZE = 15        # fits the mono advance into ~8px after centering
Y_OFFSET = -2         # nudge the glyph baseline within the cell
THRESHOLD = 110       # luma cutoff for a "set" pixel (0..255)
FIRST, LAST = 0x20, 0x7E

def render_glyph(font, ch):
    # Render the character oversized, then center its ink box into the 8x16 cell.
    big = Image.new("L", (CELL_W * 3, CELL_H * 3), 0)
    d = ImageDraw.Draw(big)
    d.text((CELL_W, CELL_H + Y_OFFSET), ch, fill=255, font=font)
    bbox = big.getbbox()
    cell = Image.new("L", (CELL_W, CELL_H), 0)
    if bbox:
        ink = big.crop(bbox)
        # horizontal center; vertical placement keeps the original baseline row
        ox = (CELL_W - ink.width) // 2
        oy = bbox[1] - CELL_H
        cell.paste(ink, (ox, oy))
    rows = []
    px = cell.load()
    for y in range(CELL_H):
        bits = 0
        for x in range(CELL_W):
            if px[x, y] >= THRESHOLD:
                bits |= 1 << (7 - x)
        rows.append(bits)
    return rows

def build():
    font = ImageFont.truetype(FONT_PATH, FONT_SIZE)
    data = bytearray(256 * CELL_H)
    for code in range(FIRST, LAST + 1):
        rows = render_glyph(font, chr(code))
        for i, b in enumerate(rows):
            data[code * CELL_H + i] = b
    return data

def dump(data, s):
    for code in [ord(c) for c in s]:
        print(f"--- '{chr(code)}' (0x{code:02x}) ---")
        for i in range(CELL_H):
            b = data[code * CELL_H + i]
            print("".join("#" if b & (1 << (7 - x)) else "." for x in range(CELL_W)))

if __name__ == "__main__":
    data = build()
    if len(sys.argv) > 1 and sys.argv[1] == "--dump":
        dump(data, sys.argv[2] if len(sys.argv) > 2 else "Ag@0")
    else:
        out = "lisp/data/font8x16.bin"
        with open(out, "wb") as f:
            f.write(data)
        print(f"wrote {out} ({len(data)} bytes)")
