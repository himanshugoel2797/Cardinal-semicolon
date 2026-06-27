// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// Host test for the virtio-input driver (lisp/drivers/virtio-input.clp). The
// driver's HARDWARE half (virtio bring-up, MSI pump, eventq DMA) is not
// host-testable -- it needs kernel mmio/dma/MSI prims -- but the driver factors
// its evdev DECODE into PURE functions:
//
//   parse-input-event -- one virtio_input_event {u16 type; u16 code; u32 value}
//                        out of an 8-byte LE buffer, value SIGN-EXTENDED.
//   reduce-event / reduce-events -- fold a sequence of (type code value) events
//                        into the coreinput payloads (pointer x y down?) /
//                        (key code value), flushed on EV_SYN.
//
// We register EMPTY stub sys-mmio/sys-pci modules so (import virtio-input) (which
// transitively imports virtio -> sys-mmio/sys-pci) resolves; the hardware prims
// are never CALLED because we only drive the pure functions. Source modules
// (driver-util, virtio, virtio-input) load from the lisp/ tree via the loader,
// modelled on test_compositor.c.
//
// argv[1] is the test dir; from it we derive the lisp/ tree.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lisp.h"

static char g_lispdir[1024];
static const char *const BASES[] = {"drivers", "lib", "servers"};

static bool vin_loader(const char *name, const char **src, size_t *len, void *ctx) {
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

// Empty capability stubs: a host has no port I/O / MMIO / PCI. The single dummy
// export makes the module non-empty so import binds something; none of these are
// ever called by the pure decode path under test.
static lisp_value stub(lisp_value *a, int n, const char **e) {
    (void)a;
    (void)n;
    (void)e;
    return LISP_FALSE;
}
static const lisp_builtin_export STUB[] = {{"__stub", stub}};

static const char *PROG =
    "(import virtio-input driver-util)"
    "(define t (spawn (lambda ()"
    "  (define fails '())"
    "  (define (ck name got want)"
    "    (if (equal? got want) #t (set! fails (cons (list name got want) fails))))"

    // --- 1. parse-input-event: type/code/value out of an 8-byte LE record,
    // value sign-extended. Build a buffer, poke the 3 fields, read them back. ---
    "  (define b (make-bytes 8))"
    // EV_ABS(3) code ABS_X(0) value 1234 -> (3 0 1234)
    "  (bytes-u16-set! b 0 3) (bytes-u16-set! b 2 0) (bytes-u32-set! b 4 1234)"
    "  (ck 'parse-abs (parse-input-event b 0) (list 3 0 1234))"
    // EV_REL(2) code REL_Y(1) value -3 (0xFFFFFFFD) -> sign-extended to -3
    "  (bytes-u16-set! b 0 2) (bytes-u16-set! b 2 1) (bytes-u32-set! b 4 #xFFFFFFFD)"
    "  (ck 'parse-rel-neg (parse-input-event b 0) (list 2 1 -3))"
    // EV_KEY(1) code 30 (a key) value 1 (press)
    "  (bytes-u16-set! b 0 1) (bytes-u16-set! b 2 30) (bytes-u32-set! b 4 1)"
    "  (ck 'parse-key (parse-input-event b 0) (list 1 30 1))"
    // parse honours the offset arg (used by the pump to index into the ring buf).
    "  (define b2 (make-bytes 16))"
    "  (bytes-u16-set! b2 8 3) (bytes-u16-set! b2 10 1) (bytes-u32-set! b2 12 77)"
    "  (ck 'parse-off (parse-input-event b2 8) (list 3 1 77))"

    // --- 2. the reducer over mock event streams. reduce-events returns
    // (final-pstate payloads); we assert the payloads (cadr). ---

    // TABLET: ABS_X, ABS_Y, BTN_LEFT down, EV_SYN -> one (pointer 100 200 #t).
    "  (ck 'tablet-press"
    "      (cadr (reduce-events 0 0 (list"
    "        (list EV-ABS ABS-X 100)"
    "        (list EV-ABS ABS-Y 200)"
    "        (list EV-KEY BTN-LEFT 1)"
    "        (list EV-SYN 0 0))))"
    "      (list (list 'pointer 100 200 #t)))"

    // TABLET motion only (no button) -> pointer with down? carried (#f initially).
    "  (ck 'tablet-move"
    "      (cadr (reduce-events 0 0 (list"
    "        (list EV-ABS ABS-X 50) (list EV-ABS ABS-Y 60)"
    "        (list EV-SYN 0 0))))"
    "      (list (list 'pointer 50 60 #f)))"

    // KEYBOARD: EV_KEY code 30 press, EV_SYN -> (key 30 1); no pointer payload
    // (a pure key frame has no pointer activity).
    "  (ck 'key-press"
    "      (cadr (reduce-events 0 0 (list"
    "        (list EV-KEY 30 1) (list EV-SYN 0 0))))"
    "      (list (list 'key 30 1)))"
    "  (ck 'key-release"
    "      (cadr (reduce-events 0 0 (list"
    "        (list EV-KEY 30 0) (list EV-SYN 0 0))))"
    "      (list (list 'key 30 0)))"

    // MOUSE: relative deltas accumulate onto the running x/y; negative delta
    // sign-extends correctly via parse, but here we feed the already-signed value.
    "  (ck 'mouse-rel"
    "      (cadr (reduce-events 10 10 (list"
    "        (list EV-REL REL-X 5) (list EV-REL REL-Y -3)"
    "        (list EV-SYN 0 0))))"
    "      (list (list 'pointer 15 7 #f)))"

    // BUTTON-ONLY frame (no motion): BTN_LEFT down at last x/y -> (pointer x y #t).
    "  (ck 'button-only"
    "      (cadr (reduce-events 40 50 (list"
    "        (list EV-KEY BTN-LEFT 1) (list EV-SYN 0 0))))"
    "      (list (list 'pointer 40 50 #t)))"

    // MULTI-FRAME: a key frame then a pointer frame -> both payloads, in order,
    // and x/y/down persist across the EV_SYN boundary (frame 2 sees the button
    // still up, pointer at the new abs coords).
    "  (ck 'multi-frame"
    "      (cadr (reduce-events 0 0 (list"
    "        (list EV-KEY 16 1) (list EV-SYN 0 0)"          // frame 1: key 16 press
    "        (list EV-ABS ABS-X 7) (list EV-ABS ABS-Y 8)"
    "        (list EV-KEY BTN-LEFT 1) (list EV-SYN 0 0))))" // frame 2: move + press
    "      (list (list 'key 16 1) (list 'pointer 7 8 #t)))"

    // BUTTON STATE PERSISTS: press in frame 1, then motion-only frame 2 keeps the
    // button held in the emitted pointer.
    "  (ck 'held-across-frames"
    "      (cadr (reduce-events 0 0 (list"
    "        (list EV-KEY BTN-LEFT 1) (list EV-SYN 0 0)"
    "        (list EV-ABS ABS-X 9) (list EV-ABS ABS-Y 9) (list EV-SYN 0 0))))"
    "      (list (list 'pointer 0 0 #t) (list 'pointer 9 9 #t)))"

    // A key AND a pointer change in the SAME frame -> key first, then pointer.
    "  (ck 'key-and-pointer-same-frame"
    "      (cadr (reduce-events 0 0 (list"
    "        (list EV-KEY 44 1) (list EV-ABS ABS-X 3) (list EV-ABS ABS-Y 4)"
    "        (list EV-SYN 0 0))))"
    "      (list (list 'key 44 1) (list 'pointer 3 4 #f)))"

    "  (if (null? fails) 'ALLOK (cons 'FAILED (reverse fails))))))";

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    snprintf(g_lispdir, sizeof g_lispdir, "%s/../../../lisp", dir);

    uintptr_t stack_marker;
    lisp_gc_init(&stack_marker);
    lisp_value env = lisp_default_env();
    lisp_install_sched(env);
    lisp_set_module_loader(vin_loader, NULL);

    // Register the capability stubs BEFORE any import resolves them.
    lisp_register_builtin_module(env, "sys-mmio", STUB, 1);
    lisp_register_builtin_module(env, "sys-pci", STUB, 1);

    printf("[lisp virtio-input] parse-input-event + evdev reducer\n");

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
        printf("  ok   event parse (LE, sign-extended) + tablet/mouse/keyboard\n");
        printf("       reduction to (pointer ...) / (key ...) coreinput payloads\n");
        return 0;
    }
    printf("  FAIL state=%d err=%s\n  result: %s\n", lisp_ctx_state(t),
           err ? err : "(none)", buf);
    return 1;
}
