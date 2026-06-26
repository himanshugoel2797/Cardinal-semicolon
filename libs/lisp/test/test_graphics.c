// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// Host test for the 2D graphics library (lisp/lib/graphics.clp) + bitmap-font
// renderer (lisp/lib/font.clp) and the gfx-*! C primitives they sit on. All of it
// is PURE pixel math -- draw into a plain make-bytes buffer and assert the pixel
// words back -- exactly the part QEMU can't check deterministically, so we drive it
// on the host the way test_usb does the USB descriptor parsers.
//
// argv[1] is the test directory (libs/lisp/test); the lisp/ tree is derived from it
// so the module loader can resolve graphics + font + driver-util.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lisp.h"

static char g_lispdir[1024];
static const char *const BASES[] = {"servers", "lib", "drivers"};

static bool gfx_loader(const char *name, const char **src, size_t *len, void *ctx) {
    (void)ctx;
    for (size_t b = 0; b < sizeof(BASES) / sizeof(BASES[0]); b++) {
        char path[1280];
        snprintf(path, sizeof(path), "%s/%s/%s.clp", g_lispdir, BASES[b], name);
        FILE *f = fopen(path, "rb");
        if (f == NULL)
            continue;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *buf = (char *)malloc((size_t)sz + 1);
        if (buf == NULL || fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
            fclose(f);
            return false;
        }
        buf[sz] = '\0';
        fclose(f);
        *src = buf;
        *len = (size_t)sz;
        return true;
    }
    return false;
}

static int failures = 0;
static int checks = 0;
static lisp_value g_env;

static void chk(const char *src, const char *want) {
    checks++;
    const char *err = NULL;
    lisp_value r = lisp_eval_string(src, g_env, &err);
    char buf[256];
    if (err != NULL)
        snprintf(buf, sizeof buf, "error: %s", err);
    else
        lisp_print(r, buf, sizeof buf);
    if (strcmp(buf, want) != 0) {
        printf("  FAIL %-58s got '%s' want '%s'\n", src, buf, want);
        failures++;
    } else {
        printf("  ok   %-58s -> %s\n", src, buf);
    }
}

static void run(const char *src) {
    const char *err = NULL;
    lisp_eval_string(src, g_env, &err);
    if (err != NULL) {
        printf("  FAIL (setup) %s -> error: %s\n", src, err);
        failures++;
    }
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    snprintf(g_lispdir, sizeof g_lispdir, "%s/../../../lisp", dir);

    uintptr_t stack_marker;
    lisp_gc_init(&stack_marker);
    g_env = lisp_default_env();
    lisp_install_sched(g_env);
    lisp_set_module_loader(gfx_loader, NULL);

    printf("[lisp gfx] graphics primitives + bitmap-font rendering\n");

    const char *err = NULL;
    lisp_eval_string("(import graphics)", g_env, &err);
    if (err == NULL) lisp_eval_string("(import font)", g_env, &err);
    if (err != NULL) {
        printf("  FAIL import -> error: %s\n", err);
        return 1;
    }

    // A 16x12 surface; stride deliberately wider than width*4 (padded rows) to
    // catch stride/width confusion. Pixels are 0x00RRGGBB.
    run("(define W 16)");
    run("(define H 12)");
    run("(define STRIDE 80)");          // 80 > 16*4 = 64: padded
    run("(define buf (make-bytes (* STRIDE H)))");
    run("(define s (make-surface buf W H STRIDE))");

    // --- clear / fill-rect / clipping ---
    run("(clear s 0)");
    chk("(get-pixel s 0 0)", "0");
    chk("(get-pixel s 15 11)", "0");
    run("(fill-rect s 2 1 3 2 16711680)");                 // red 0xFF0000 at (2,1) 3x2
    chk("(get-pixel s 2 1)", "16711680");
    chk("(get-pixel s 4 2)", "16711680");                  // bottom-right of the rect
    chk("(get-pixel s 5 1)", "0");                         // just past the right edge
    chk("(get-pixel s 2 3)", "0");                         // just past the bottom edge
    chk("(get-pixel s 1 1)", "0");                         // just left
    // clip: a rect running off the right/bottom must not crash and must paint only
    // the on-surface part.
    run("(fill-rect s 14 10 99 99 65280)");                // green, extends off-surface
    chk("(get-pixel s 15 11)", "65280");                   // bottom-right corner painted
    chk("(get-pixel s 14 10)", "65280");
    // a fully off-surface rect is a no-op (and safe).
    run("(fill-rect s 100 100 10 10 255)");
    chk("(get-pixel s 15 11)", "65280");                   // unchanged

    // --- put-pixel / hline / vline / draw-rect outline ---
    run("(clear s 0)");
    run("(put-pixel s 7 5 255)");
    chk("(get-pixel s 7 5)", "255");
    run("(put-pixel s -1 5 255)");                         // off-surface: ignored, no crash
    run("(put-pixel s 99 5 255)");
    run("(draw-hline s 1 0 5 255)");
    chk("(get-pixel s 1 0)", "255");
    chk("(get-pixel s 5 0)", "255");
    chk("(get-pixel s 6 0)", "0");
    run("(draw-vline s 0 1 4 255)");
    chk("(get-pixel s 0 4)", "255");
    chk("(get-pixel s 0 5)", "0");
    run("(clear s 0)");
    run("(draw-rect s 1 1 6 6 1 16711680)");               // 1px outline, 6x6 at (1,1)
    chk("(get-pixel s 1 1)", "16711680");                  // top-left corner
    chk("(get-pixel s 6 6)", "16711680");                  // bottom-right corner
    chk("(get-pixel s 3 3)", "0");                         // hollow interior
    chk("(get-pixel s 3 1)", "16711680");                  // top edge
    chk("(get-pixel s 1 4)", "16711680");                  // left edge

    // --- line (Bresenham) ---
    run("(clear s 0)");
    run("(draw-line s 0 0 5 5 255)");                      // perfect diagonal
    chk("(get-pixel s 0 0)", "255");
    chk("(get-pixel s 3 3)", "255");
    chk("(get-pixel s 5 5)", "255");
    chk("(get-pixel s 1 0)", "0");                         // off the diagonal

    // --- filled circle ---
    run("(clear s 0)");
    run("(fill-circle s 7 6 3 65280)");                    // center (7,6) r=3
    chk("(get-pixel s 7 6)", "65280");                     // center filled
    chk("(get-pixel s 7 3)", "65280");                     // top of disc (dy=-3)
    chk("(get-pixel s 9 6)", "65280");                     // right extent (dx=3)
    chk("(get-pixel s 11 6)", "0");                        // outside the disc

    // --- opaque blit ---
    run("(define ibuf (make-bytes (* 16 3)))");            // 4x3 image, stride 16
    run("(define img (make-surface ibuf 4 3 16))");
    run("(clear img 16711680)");                           // solid red image
    run("(clear s 0)");
    run("(blit s img 5 4)");                               // place at (5,4)
    chk("(get-pixel s 5 4)", "16711680");
    chk("(get-pixel s 8 6)", "16711680");                  // bottom-right of the image
    chk("(get-pixel s 9 4)", "0");                         // just past the image
    chk("(get-pixel s 4 4)", "0");

    // --- alpha blit (source-over) ---
    run("(clear s 0)");                                    // dst black
    run("(clear img (argb img 128 255 255 255))");         // 50% white image (a=128)
    run("(blit-alpha s img 0 0)");
    // 255*128 over 0: (255*128 + 0 + 127)/255 = 128 per channel -> 0x808080
    chk("(get-pixel s 0 0)", "8421504");                   // 0x808080
    chk("(get-pixel s 3 2)", "8421504");
    // a fully transparent (a=0) source leaves the dst untouched.
    run("(clear s 16711680)");
    run("(clear img (argb img 0 0 255 0))");
    run("(blit-alpha s img 0 0)");
    chk("(get-pixel s 0 0)", "16711680");                  // unchanged

    // --- glyph blit (synthetic 1bpp font) ---
    // Build a 256-glyph 8x16 plane; set glyph 'A' (65): row0 = 0xFF (all 8),
    // row1 = 0x81 (leftmost + rightmost), rest 0.
    run("(define fb (make-bytes (* 256 16)))");
    run("(bytes-u8-set! fb (* 65 16) 255)");
    run("(bytes-u8-set! fb (+ (* 65 16) 1) 129)");
    run("(define fnt (make-font fb 8 16))");
    run("(define s2 (make-surface buf W H STRIDE))");
    run("(clear s2 0)");
    run("(draw-char s2 fnt 0 0 (integer->char 65) 255 0 #f 1)");  // blue fg, transparent bg
    chk("(get-pixel s2 0 0)", "255");                      // row0 bit7 (leftmost) set
    chk("(get-pixel s2 7 0)", "255");                      // row0 bit0 (rightmost) set
    chk("(get-pixel s2 0 1)", "255");                      // row1 leftmost set
    chk("(get-pixel s2 3 1)", "0");                        // row1 middle clear (transparent)
    chk("(get-pixel s2 7 1)", "255");                      // row1 rightmost set
    chk("(get-pixel s2 0 2)", "0");                        // row2 all clear
    // opaque background: clear bits now paint bg.
    run("(clear s2 0)");
    run("(draw-char s2 fnt 0 0 (integer->char 65) 255 16711680 #t 1)");  // bg red
    chk("(get-pixel s2 3 1)", "16711680");                 // clear bit -> bg
    chk("(get-pixel s2 0 0)", "255");                      // set bit -> fg

    // --- glyph scaling (2x): each source pixel -> 2x2 block ---
    run("(clear s2 0)");
    run("(draw-char s2 fnt 0 0 (integer->char 65) 255 0 #f 2)");
    chk("(get-pixel s2 0 0)", "255");                      // (0,0) source pixel
    chk("(get-pixel s2 1 1)", "255");                      // still within the (0,0) 2x2 block
    chk("(get-pixel s2 14 0)", "255");                     // source x=7 -> dst x=14,15
    chk("(get-pixel s2 15 0)", "255");

    // --- draw-text + text-width ---
    run("(bytes-u8-set! fb (* 66 16) 255)");               // give 'B' a row0 too
    run("(clear s2 0)");
    run("(draw-text s2 fnt 0 0 \"AB\" 255 0 #f 1)");
    chk("(get-pixel s2 0 0)", "255");                      // 'A' at column 0
    chk("(get-pixel s2 8 0)", "255");                      // 'B' advanced one 8px cell
    chk("(text-width fnt \"AB\" 1)", "16");                // 2 chars * 8px
    chk("(text-width fnt \"AB\" 2)", "32");
    chk("(text-width fnt \"A\nBBB\" 1)", "24");            // longest line (BBB) * 8

    // --- HW-2D backend override: a surface whose 'fill-rect routes to a hook ---
    // The hook only counts; it does NOT touch the framebuffer. So after a fill, the
    // pixel must be UNCHANGED (proving software didn't run) and the counter bumped.
    run("(define hit 0)");
    run("(define bs (make-surface* buf W H STRIDE 16 8 0 (list (cons 'fill-rect (lambda (s x y w h c) (set! hit (+ hit 1)))))))");
    run("(clear s 7)");                                    // s shares buf with bs: all pixels = 7
    run("(fill-rect bs 0 0 4 4 255)");                     // routed to the override
    chk("hit", "1");                                       // the override ran
    chk("(get-pixel bs 0 0)", "7");                        // software fill did NOT paint

    // --- adversarial: overflow / OOB must be rejected, not corrupt memory ---
    // A huge (4-aligned) stride whose (last-row*stride) overflows int64 must be
    // caught by the overflow-safe bounds check rather than wrapping and passing.
    chk("(gfx-fill-rect! (make-bytes 64) 1152921504606846976 1 17 0 0 1 17 255)", "error: gfx-fill-rect!: out of bounds");
    // A non-4-aligned stride is rejected (32-bit stores need alignment).
    chk("(gfx-fill-rect! (make-bytes 64) 7 1 1 0 0 1 1 255)", "error: gfx-fill-rect!: data/stride not 4-aligned");
    chk("(gfx-fill-rect! (make-bytes 16) 4 1 100 0 0 1 100 255)", "error: gfx-fill-rect!: out of bounds");
    // glyph with an over-large gh (would overflow gh*rowbytes) is rejected.
    chk("(gfx-glyph! (make-bytes 64) 4 4 4 0 0 (make-bytes 16) 0 8 1000000 255 0 0 1)", "error: gfx-glyph!: bad geometry");
    // out-of-range character is skipped (no error, draws nothing).
    run("(clear s2 0)");
    run("(draw-char s2 fnt 0 0 (integer->char 9000) 255 0 #f 1)");  // code > 255 glyphs
    chk("(get-pixel s2 0 0)", "0");                                 // skipped, nothing drawn
    run("(draw-char s2 fnt 0 0 (integer->char 65) 255 0 #f 1)");    // in-range 'A' still works
    chk("(get-pixel s2 0 0)", "255");

    printf("\n[lisp gfx] %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
