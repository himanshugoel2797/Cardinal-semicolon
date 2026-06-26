// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// ttf -- a thin integer-ABI wrapper over stb_truetype (libs/stb), built as its own
// SSE-enabled static library because the rasterizer is float-based and the kernel
// modules are compiled -mno-sse. SSE is enabled at runtime (SysFP clears CR0.EM,
// sets OSFXSR), and ONLY integer/pointer values cross this boundary (no float in
// any signature), so a -mno-sse caller links and calls these safely; the float math
// stays inside this TU. Rasterization runs as a non-yielding Lisp primitive, so the
// XMM state it uses is never observed by other code.
//
// The font is stateless here: each call (re)parses the TTF header (cheap) from the
// caller's font bytes -- no persistent handle. A glyph cache in Lisp (lisp/lib/ttf)
// makes that per-glyph, not per-draw.

#ifndef CARDINAL_TTF_H
#define CARDINAL_TTF_H

#include <stdint.h>
#include <stddef.h>

// Rasterize `codepoint` from the TTF in `font` (`fontlen` bytes) at `px` pixel
// height into an 8-bit coverage bitmap (0=transparent..255=opaque). On success
// returns a malloc'd `*w * *h` buffer and fills the glyph's pen offsets (*xoff left,
// *yoff top-from-baseline, both may be negative) and horizontal *advance (pixels).
// Returns NULL for an empty glyph (e.g. space) -- *advance is still set, *w/*h = 0.
// Free the returned buffer with ttf_free_bitmap. Returns NULL (and zeroes outputs)
// if the font can't be parsed.
uint8_t *ttf_rasterize(const uint8_t *font, size_t fontlen, int codepoint, int px,
                       int *w, int *h, int *xoff, int *yoff, int *advance);
void ttf_free_bitmap(uint8_t *bmp);

// Vertical metrics at `px` pixel height (pixels): ascent (>0, baseline to top),
// descent (<0, baseline to bottom), linegap. Line pitch = ascent - descent + linegap.
void ttf_vmetrics(const uint8_t *font, size_t fontlen, int px,
                  int *ascent, int *descent, int *linegap);

#endif
