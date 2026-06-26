// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// The stb_truetype implementation + a minimal libm, isolated in this SSE-enabled TU
// (see ttf.h). Under -nostdinc we must define every STBTT_* hook so stb pulls in no
// standard headers: allocation -> the kernel `malloc`/`free` (common/), the bulk
// ops -> `memcpy`/`memset`, and the floating-point helpers below (the kernel has no
// libm). The trig/pow helpers are only reached by stb's SDF path, which we never
// call; they are safe placeholders.

#include <stdint.h>
#include <stddef.h>

// Provided by common/ (declared here since we build -nostdinc).
extern void *memcpy(void *, const void *, size_t);
extern void *memset(void *, int, size_t);
extern size_t strlen(const char *);

// stb's allocator: a per-rasterize BUMP ARENA, not the kernel heap. stb makes many
// small alloc/free calls per glyph; churning the kernel allocator at that rate from
// the Lisp-eval context hangs it. stb frees everything within one GetGlyphBitmap, so
// resetting the arena per ttf_rasterize call suffices (and the returned bitmap lives
// in the arena until the caller copies it out -- which the sys-ttf prim does before
// the next call). 1 MiB dwarfs a large glyph's edge list + bitmap. The arena is a
// single shared buffer, so the prim serialises ttf_rasterize across cores (a lock)
// -- the one UI context never re-enters it anyway.
#define TTF_ARENA_SZ (1u << 20)
static unsigned char ttf_arena[TTF_ARENA_SZ];
static unsigned long ttf_arena_off;
static void *ttf_alloc(unsigned long n) {
    n = (n + 15ul) & ~15ul;
    if (ttf_arena_off + n > TTF_ARENA_SZ) return (void *)0;   // OOM -> stb tolerates NULL
    void *p = ttf_arena + ttf_arena_off;
    ttf_arena_off += n;
    return p;
}

// Square root via the SSE instruction directly (inline asm), NOT __builtin_sqrt:
// at -O0 (the kernel's Debug build) the builtin lowers to a CALL to `sqrt`, which --
// since we also DEFINE sqrt below for the freestanding link -- would recurse forever
// (the original hang). The asm form is always the bare sqrtsd/sqrtss instruction.
static inline double sse_sqrt(double x) { double r; __asm__("sqrtsd %1, %0" : "=x"(r) : "x"(x)); return r; }
static inline float  sse_sqrtf(float x) { float r;  __asm__("sqrtss %1, %0" : "=x"(r) : "x"(x)); return r; }

// A freestanding (kernel) build has no libm, but the compiler may still emit a call
// to sqrt/sqrtf (notably folding `(float)sqrt((double)x)` to `sqrtf(x)`). Supply them
// via the instruction directly. A hosted build (the host test) gets them from libm.
#if !__STDC_HOSTED__
double sqrt(double x) { return sse_sqrt(x); }
float sqrtf(float x)  { return sse_sqrtf(x); }
#endif

// --- minimal libm (this TU has SSE2, so doubles are real) --------------------
static double ttf_floor(double x) { double t = (double)(long long)x; return (t > x) ? t - 1.0 : t; }
static double ttf_ceil(double x)  { double t = (double)(long long)x; return (t < x) ? t + 1.0 : t; }
static double ttf_fabs(double x)  { return x < 0 ? -x : x; }
static double ttf_sqrt(double x)  { return sse_sqrt(x); }
static double ttf_fmod(double x, double y) { return (y == 0.0) ? 0.0 : (x - ttf_floor(x / y) * y); }
// SDF-only (unreached by plain bitmap rasterization): safe placeholders.
static double ttf_pow(double x, double y)  { (void)y; return x; }
static double ttf_cos(double x)            { (void)x; return 1.0; }
static double ttf_acos(double x)           { (void)x; return 0.0; }

#define STBTT_malloc(x, u)  ((void)(u), ttf_alloc(x))
#define STBTT_free(x, u)    ((void)(x), (void)(u))   // bump arena: reclaimed en masse
#define STBTT_assert(x)     ((void)0)
#define STBTT_strlen(x)     strlen(x)
#define STBTT_memcpy        memcpy
#define STBTT_memset        memset
#define STBTT_ifloor(x)     ((int)ttf_floor(x))
#define STBTT_iceil(x)      ((int)ttf_ceil(x))
#define STBTT_sqrt(x)       ttf_sqrt(x)
#define STBTT_pow(x, y)     ttf_pow(x, y)
#define STBTT_fmod(x, y)    ttf_fmod(x, y)
#define STBTT_cos(x)        ttf_cos(x)
#define STBTT_acos(x)       ttf_acos(x)
#define STBTT_fabs(x)       ttf_fabs(x)

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include "ttf.h"

uint8_t *ttf_rasterize(const uint8_t *font, size_t fontlen, int codepoint, int px,
                       int *w, int *h, int *xoff, int *yoff, int *advance) {
    (void)fontlen;
    *w = *h = *xoff = *yoff = *advance = 0;
    ttf_arena_off = 0;   // reclaim everything the previous call used
    stbtt_fontinfo fi;
    if (px <= 0 || !stbtt_InitFont(&fi, font, stbtt_GetFontOffsetForIndex(font, 0)))
        return NULL;
    float scale = stbtt_ScaleForPixelHeight(&fi, (float)px);
    int g = stbtt_FindGlyphIndex(&fi, codepoint);
    int adv = 0, lsb = 0;
    stbtt_GetGlyphHMetrics(&fi, g, &adv, &lsb);
    *advance = (int)(adv * scale + 0.5f);
    // NULL for an empty glyph (space): metrics still valid, no coverage bitmap.
    return stbtt_GetGlyphBitmap(&fi, scale, scale, g, w, h, xoff, yoff);
}

void ttf_free_bitmap(uint8_t *bmp) {
    (void)bmp;   // the bitmap lives in the bump arena; reclaimed on the next call
}

void ttf_vmetrics(const uint8_t *font, size_t fontlen, int px,
                  int *ascent, int *descent, int *linegap) {
    (void)fontlen;
    *ascent = *descent = *linegap = 0;
    stbtt_fontinfo fi;
    if (px <= 0 || !stbtt_InitFont(&fi, font, stbtt_GetFontOffsetForIndex(font, 0)))
        return;
    float scale = stbtt_ScaleForPixelHeight(&fi, (float)px);
    int a = 0, d = 0, l = 0;
    stbtt_GetFontVMetrics(&fi, &a, &d, &l);
    *ascent  = (int)(a * scale + 0.5f);
    *descent = (int)(d * scale - 0.5f);
    *linegap = (int)(l * scale + 0.5f);
}
