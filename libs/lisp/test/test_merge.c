// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// Host test for the phase-7 two-level layer merge primitives (libs/lisp/src/prims.c,
// notes/servers/CoreCompositor.md "Sharded per-core compositor"). The merge is the
// algorithmic heart of the sharded compositor and the one place correctness is
// subtle, so it is proven here in isolation -- pure prims over hand-built planes,
// no IPC/cores -- before the distributed owner/shard wiring is built on top.
//
//   - gfx-zpick!   the opaque pass: fold N shard layers into one image + a per-pixel
//                  topmost-opaque z (Zop) by a max-z pick. The whole point is that it
//                  is SHARDING-ORDER-INDEPENDENT: the same windows merged in any order
//                  give the same result (a max pick is commutative/associative).
//   - gfx-blend-z! the translucent pass: alpha-over a translucent window only where
//                  its z is ABOVE Zop (in front of the nearest opaque surface there);
//                  below Zop it is occluded and skipped.

#include <stdio.h>
#include <string.h>

#include "lisp.h"

static const char *PROG =
    // An 8-pixel "screen": 4 wide x 2 high for the positioned blend-z test; the
    // zpick test treats it as a flat 8-pixel plane. 4 bytes/pixel, z plane likewise.
    "(define NP 8)"
    "(define (plane) (make-bytes (* NP 4)))"     // a zeroed 8-pixel plane
    "(define (px b p) (bytes-u32-ref b (* p 4)))"
    "(define (setpx! b p v) (bytes-u32-set! b (* p 4) v))"
    "(define fails '())"
    "(define (ck name got want) (if (equal? got want) #t (set! fails (cons (list name got want) fails))))"
    "(define (run)"
    // --- 1. gfx-zpick! max-z pick + sharding-order independence ---
    // Three single-pixel-ish layers contributing to pixels 0 and 1 with different
    // z. The max-z contributor must win each pixel, regardless of merge order.
    "  (define (layer a-c a-z) (cons a-c a-z))"   // (color-plane . z-plane)
    "  (define (mk p0c p0z p1c p1z)"
    "    (let ((c (plane)) (z (plane)))"
    "      (if (> p0z 0) (begin (setpx! c 0 p0c) (setpx! z 0 p0z)))"
    "      (if (> p1z 0) (begin (setpx! c 1 p1c) (setpx! z 1 p1z)))"
    "      (cons c z)))"
    "  (define L1 (mk #x111111 10 0 0))"          // px0 z10
    "  (define L2 (mk #x222222 30 #x0000cc 5))"   // px0 z30 (highest), px1 z5
    "  (define L3 (mk #x333333 20 #x0000ee 25))"  // px0 z20, px1 z25 (highest)
    "  (define (merge order)"
    "    (let ((ac (plane)) (az (plane)))"
    "      (for-each (lambda (L) (gfx-zpick! ac az (car L) (cdr L) NP)) order)"
    "      ac))"
    "  (let ((m1 (merge (list L1 L2 L3)))"        // one shard order
    "        (m2 (merge (list L3 L1 L2))))"       // a different shard order
    "    (ck 'zpick-px0       (px m1 0) #x222222)" // max z=30 -> L2's colour
    "    (ck 'zpick-px1       (px m1 1) #x0000ee)" // max z=25 -> L3's colour
    "    (ck 'zpick-order-px0 (px m2 0) (px m1 0))" // order-independent
    "    (ck 'zpick-order-px1 (px m2 1) (px m1 1))"
    "    (ck 'zpick-empty     (px m1 2) 0)"        // no contributor -> stays 0
    "    (ck 'zpick-order-px2 (px m2 2) 0))"       // ...in either order
    // An all-empty (z==0) layer must not disturb an accumulator.
    "  (let ((ac (plane)) (az (plane)) (empty (mk 0 0 0 0)))"
    "    (setpx! ac 3 #x777777) (setpx! az 3 99)"
    "    (gfx-zpick! ac az (car empty) (cdr empty) NP)"
    "    (ck 'zpick-empty-noop (px ac 3) #x777777))"
    // --- 2. gfx-blend-z! z-gated translucent alpha-over ---
    // dst 4x2 (stride 16). Zop: px0=50, px1=10. A 2x1 translucent window at (0,0),
    // wz=20: px0 occluded (20<=50), px1 in front (20>10) so it composites.
    "  (let ((dst (plane)) (zb (plane)) (src (make-bytes (* 2 4))))"
    "    (setpx! zb 0 50) (setpx! zb 1 10)"
    "    (bytes-u32-set! src 0 #xff808080)"        // px (0,0): alpha 255, grey
    "    (bytes-u32-set! src 4 #xff808080)"        // px (1,0): alpha 255, grey
    "    (gfx-blend-z! dst 16 4 2 0 0 src 8 2 1 zb 20)"
    "    (ck 'blendz-occluded (px dst 0) 0)"        // wz<=Zop -> untouched
    "    (ck 'blendz-infront  (px dst 1) #x808080))" // wz>Zop -> grey (alpha 255 -> low3 replaced)
    // wz=0 is occluded everywhere (0 <= any Zop, incl. an empty Zop of 0): a
    // window at the bottom of the z-order composites nothing.
    "  (let ((dst (plane)) (zb (plane)) (src (make-bytes 4)))"
    "    (bytes-u32-set! src 0 #xffffffff)"
    "    (gfx-blend-z! dst 16 4 2 0 0 src 4 1 1 zb 0)"
    "    (ck 'blendz-wz0-occluded (px dst 0) 0))"
    // partial alpha where in front: white @ alpha 128 over black -> ~0x808080.
    "  (let ((dst (plane)) (zb (plane)) (src (make-bytes 4)))"
    "    (bytes-u32-set! src 0 #x80ffffff)"        // alpha 128, white
    "    (gfx-blend-z! dst 16 4 2 0 0 src 4 1 1 zb 5)" // Zop=0 everywhere, wz=5 > 0
    "    (ck 'blendz-alpha (px dst 0) #x808080))"
    "  (if (null? fails) 'ALLOK (cons 'FAILED (reverse fails))))"
    "(run)";

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    uintptr_t stack_marker;
    lisp_gc_init(&stack_marker);
    lisp_value env = lisp_default_env();

    printf("[lisp merge] gfx-zpick! / gfx-blend-z! (phase-7 two-level merge)\n");

    const char *err = NULL;
    lisp_value r = lisp_eval_string(PROG, env, &err);
    char buf[1024];
    lisp_print(r, buf, sizeof buf);
    if (err != NULL || strcmp(buf, "ALLOK") != 0) {
        printf("  FAIL err=%s\n  result: %s\n", err ? err : "(none)", buf);
        return 1;
    }
    // A negative wz must be REFUSED, not truncated to ~0u (which would pass the
    // `wz > Zop` gate at every pixel and let a window composite over everything
    // regardless of z). Assert the prim errors rather than rendering.
    const char *werr = NULL;
    lisp_eval_string(
        "(let ((dst (make-bytes 32)) (zb (make-bytes 32)) (src (make-bytes 4)))"
        "  (gfx-blend-z! dst 16 4 2 0 0 src 4 1 1 zb -1))",
        env, &werr);
    if (werr == NULL) {
        printf("  FAIL negative wz was accepted (occlusion bypass)\n");
        return 1;
    }
    printf("  ok   z-pick is max-z + sharding-order independent; blend-z gates on Zop\n");
    printf("  ok   negative wz refused (no occlusion bypass)\n");
    return 0;
}
