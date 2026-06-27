// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// Host test for the PS/2 mouse decode (lisp/drivers/ps2.clp). The hardware bring-
// up (i8042 aux init, IRQ12, the port-drain pump) needs an i8042 and cannot run on
// the host -- and QEMU does NOT deliver synthetic mouse input to the emulated PS/2
// controller anyway (project notes), so the interrupt/packet-framing path is only
// exercisable on real hardware. What IS host-testable is the PURE packet math,
// which ps2.clp exports for exactly this reason:
//
//   (ps2-mouse-decode flags b1 b2) -> (dx dy left? right? middle?)
//   (ps2-mouse-accum  x y dx dy)    -> (new-x new-y)   clamped to the default space
//
// We register EMPTY stub sys-io / sys-irq builtin modules so ps2.clp's
// (import sys-io sys-irq) succeeds on the host (the driver references no port-I/O
// or IRQ prim at decode time), import ps2, and assert the decode + accumulate over
// mock packet bytes. argv[1] is the test dir; from it we derive the lisp/ tree.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lisp.h"

static char g_lispdir[1024];
static const char *const BASES[] = {"drivers", "lib", "servers"};

static bool ps2_loader(const char *name, const char **src, size_t *len, void *ctx) {
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

// An empty builtin module body: ps2.clp imports sys-io / sys-irq for their port-
// I/O and IRQ prims, none of which the pure decode/accumulate functions touch. A
// single never-called stub export keeps lisp_register_builtin_module happy.
static lisp_value stub(lisp_value *a, int n, const char **e) {
    (void)a;
    (void)n;
    (void)e;
    return LISP_FALSE;
}
static const lisp_builtin_export STUB[] = {{"__stub", stub}};

static const char *PROG =
    "(import ps2)"
    "(define t (spawn (lambda ()"
    "  (define fails '())"
    "  (define (ck name got want)"
    "    (if (equal? got want) #t (set! fails (cons (list name got want) fails))))"
    // --- decode: a simple right-down-left packet. flags=0x09 = bit0(left)+bit3(1),
    // dx=+5, raw dy=+3 -> screen dy = -3, left down, no right/middle. ---
    "  (ck 'dec-basic (ps2-mouse-decode #x09 5 3) (list 5 -3 #t #f #f))"
    // X-sign set (bit4=0x10): dx is 9-bit-signed negative. flags=0x18 = bit3+bit4,
    // b1=0xFB(251) -> 251-256 = -5. raw dy=0 -> screen 0. no buttons.
    "  (ck 'dec-xneg (ps2-mouse-decode #x18 #xFB 0) (list -5 0 #f #f #f))"
    // Y-sign set (bit5=0x20): raw dy negative. flags=0x28, b2=0xFE(254) -> 254-256
    // = -2; screen dy = -(-2) = +2 (moving down). dx=+1.
    "  (ck 'dec-yneg (ps2-mouse-decode #x28 1 #xFE) (list 1 2 #f #f #f))"
    // all three buttons: flags bit0|bit1|bit2|bit3 = 0x0F. dx=0 dy=0.
    "  (ck 'dec-btns (ps2-mouse-decode #x0F 0 0) (list 0 0 #t #t #t))"
    // right-only: flags=0x0A = bit1(right)+bit3.
    "  (ck 'dec-right (ps2-mouse-decode #x0A 0 0) (list 0 0 #f #t #f))"
    // --- accumulate + clamp. The pointer space is [0,1023]x[0,767]. ---
    // from centre (512,384): move (+5,-3) -> (517, 381).
    "  (ck 'acc-basic (ps2-mouse-accum 512 384 5 -3) (list 517 381))"
    // clamp at the right/bottom edges: large positive deltas saturate.
    "  (ck 'acc-clamp-hi (ps2-mouse-accum 1020 760 100 100) (list 1023 767))"
    // clamp at the left/top edges: large negative deltas saturate at 0.
    "  (ck 'acc-clamp-lo (ps2-mouse-accum 3 2 -100 -100) (list 0 0))"
    // exact-edge values pass through unclamped.
    "  (ck 'acc-edge (ps2-mouse-accum 1023 767 0 0) (list 1023 767))"
    // --- end-to-end: decode a packet, then accumulate its delta from centre. ---
    "  (let* ((dec (ps2-mouse-decode #x09 5 3))"   // (5 -3 #t #f #f)
    "         (pos (ps2-mouse-accum 512 384 (car dec) (cadr dec))))"
    "    (ck 'e2e pos (list 517 381)))"
    "  (if (null? fails) 'ALLOK (cons 'FAILED (reverse fails))))))";

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    snprintf(g_lispdir, sizeof g_lispdir, "%s/../../../lisp", dir);

    uintptr_t stack_marker;
    lisp_gc_init(&stack_marker);
    lisp_value env = lisp_default_env();
    lisp_install_sched(env);
    lisp_register_builtin_module(env, "sys-io", STUB, 1);
    lisp_register_builtin_module(env, "sys-irq", STUB, 1);
    lisp_set_module_loader(ps2_loader, NULL);

    printf("[lisp ps2] mouse decode + accumulate (pure)\n");

    lisp_sched_t s;
    lisp_sched_init(&s, 4000000);
    s.per_context_heaps = 1;
    const char *err = NULL;
    lisp_eval_string(PROG, env, &err);
    if (err != NULL) {
        printf("  FAIL setup error: %s\n", err);
        return 1;
    }
    lisp_value t = lisp_eval_string("t", env, &err);
    lisp_sched_run(&s, 0);

    char buf[1024];
    lisp_print(lisp_ctx_value(t), buf, sizeof buf);
    int done = (lisp_ctx_state(t) == LISP_CTX_DONE);
    if (done && err == NULL && strcmp(buf, "ALLOK") == 0) {
        printf("  ok   decode (sign/buttons/Y-negate) + accumulate (clamp/edges)\n");
        return 0;
    }
    printf("  FAIL state=%d err=%s\n  result: %s\n", lisp_ctx_state(t),
           err ? err : "(none)", buf);
    return 1;
}
