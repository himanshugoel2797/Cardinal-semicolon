// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// Host test for the compositor (lisp/servers/corecompositor.clp, phases 2-3 of
// notes/servers/CoreCompositor.md). Two things are host-testable here:
//
//   1. paint-windows -- the PURE compositing core (clear + z-ordered painter's
//      blit). No caps, no IPC: we build a screen + source surfaces with known
//      colours and assert occlusion / position / background directly.
//   2. the SURFACE PROTOCOL mechanics over the real service -- connect handshake,
//      the per-client handler relay, and create/configure/commit/destroy threading
//      the surface table. The service takes its kernel authority by INJECTION, so
//      we hand it fake caps (make-bytes for backings, a tagged fake grant) and
//      assert the protocol replies + that a malformed op can't wedge the service.
//
// What stays in-OS (lisp/init.clp cardinal.compositortest): the real grant path --
// a client maps a granted backing, draws, commits, and the composited pixel is
// probed -- since zero-copy needs phys-shared grants the host can't model (a fake
// grant deep-copies on send, so the drawn pixels wouldn't reach the root's backing).
//
// argv[1] is the test dir; from it we derive the lisp/ tree for the module loader.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lisp.h"

static char g_lispdir[1024];
static const char *const BASES[] = {"servers", "lib", "drivers"};

static bool comp_loader(const char *name, const char **src, size_t *len, void *ctx) {
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

static const char *PROG =
    "(import corecompositor graphics driver-util)"
    // injected fake caps: a host has no kernel DMA/grant prims. The fake grant is a
    // distinct object so create's reply is well-formed; we never map it (host).
    "(define (falloc n) (make-bytes n))"
    "(define (fmint b p) (list 'fake-grant b))"
    "(define (frevoke g) #t)"
    // present is the phase-4 driver seam; a host has no display, so #f (off-screen
    // RAM screen). The compositor still composites; it just flushes nothing.
    "(define screen (make-surface (make-bytes (* 64 64 4)) 64 64 (* 64 4)))"
    "(define comp (start-compositor-service screen (make-compositor-caps falloc fmint frevoke #f #f)))"
    "(define t (spawn (lambda ()"
    "  (define fails '())"
    "  (define (ck name got want)"
    "    (if (equal? got want) #t (set! fails (cons (list name got want) fails))))"
    // --- 1. pure painter: occlusion + position + background ---
    "  (let* ((scr (make-surface (make-bytes (* 16 16 4)) 16 16 (* 16 4)))"
    "         (bg  (rgb scr 0 0 0))"
    "         (a   (make-surface (make-bytes (* 4 4 4)) 4 4 (* 4 4)))"
    "         (b   (make-surface (make-bytes (* 4 4 4)) 4 4 (* 4 4)))"
    "         (red (rgb a 255 0 0)) (grn (rgb b 0 255 0)))"
    "    (fill-rect a 0 0 4 4 red)"
    "    (fill-rect b 0 0 4 4 grn)"
    // back-to-front: a at (0,0), then b at (2,2) -> b occludes a in the overlap.
    "    (paint-windows scr bg (list (list a 0 0 #f) (list b 2 2 #f)))"
    "    (ck 'paint-a    (get-pixel scr 0 0)   red)"   // a only
    "    (ck 'paint-b    (get-pixel scr 2 2)   grn)"   // b only
    "    (ck 'paint-occl (get-pixel scr 3 3)   grn)"   // overlap shows b (front)
    "    (ck 'paint-bg   (get-pixel scr 10 10) bg))"   // uncovered -> background
    // --- 2. protocol mechanics over the real service (fake caps). Replies come to
    // the connected client (us), never to a field in the message. ---
    // ctx? distinguishes a real handle from data; a connect whose reply is NOT a
    // context is rejected (it would otherwise abort the serve loop on the reply
    // send) and the service survives -> the next valid connect still works.
    "  (ck 'ctx-self   (ctx? (self)) #t)"
    "  (ck 'ctx-fixnum (ctx? 7)      #f)"
    "  (send comp (list 'connect #f 7))"           // non-context reply -> ignored
    "  (send comp (list 'connect #f (self)))"
    "  (define r1 (recv))"
    "  (ck 'connected (car r1) 'connected)"
    "  (define h (cadr r1))"
    "  (send h (list 'create-surface 4 4))"
    "  (define s1 (recv))"
    "  (ck 'surface-tag    (car s1)  'surface)"
    "  (ck 'surface-id     (cadr s1) 1)"            // first id
    "  (ck 'surface-stride (nth s1 4) 16)"          // 4*4
    // configure + commit are fire-and-forget; assert they don't wedge the service:
    // a subsequent create still gets the next id.
    "  (send h (list 'configure 1 0 0 #t))"
    "  (send h (list 'commit 1 0 '()))"
    "  (send h (list 'create-surface 8 8))"
    "  (ck 'surface-id2 (cadr (recv)) 2)"
    // a malformed / out-of-range create is rejected, not crashing -- the service
    // replies an error and survives. Bad-dimension DoS vectors (non-integer,
    // non-positive, oversize) must all be refused.
    "  (send h (list 'create-surface))"                  // missing args
    "  (ck 'bad-missing (car (recv)) 'surface-error)"
    "  (send h (list 'create-surface 0 4))"              // zero dimension
    "  (ck 'bad-zero (car (recv)) 'surface-error)"
    "  (send h (list 'create-surface 99999 99999))"      // oversize -> would OOM
    "  (ck 'bad-huge (car (recv)) 'surface-error)"
    // ...and the service still works afterward (next id continues).
    "  (send h (list 'create-surface 2 2))"
    "  (ck 'survives-bad (car (recv)) 'surface)"
    // destroy acks 'ok to the owner.
    "  (send h (list 'destroy-surface 1))"
    "  (ck 'destroy-ok (recv) 'ok)"
    // --- 3. cross-client authorization: a SEPARATE client context cannot touch
    // our surfaces. Ownership is by sender identity, so the attacker must be its own
    // context (a second connect from US would carry our identity). surface 2 is ours
    // and still alive; the attacker connects, tries to destroy it, forwards us the
    // reply -- which must be a refusal, and surface 2 must survive. ---
    "  (let ((me (self)))"
    "    (spawn (lambda ()"
    "      (send comp (list 'connect #f (self)))"
    "      (let ((ah (cadr (recv))))"
    "        (send ah (list 'destroy-surface 2))"
    "        (send me (recv)))))"                    // forward the attacker's reply to us
    "    (ck 'auth-foreign-destroy (recv) (list 'destroy-error 'no-such-surface)))"
    // the owner can still destroy it -> the attacker did not affect it.
    "  (send h (list 'destroy-surface 2))"
    "  (ck 'owner-destroy-ok (recv) 'ok)"
    // --- 4. input routing + drag-move (phase 6). Drive (input ev) on the primary
    // mailbox -- exactly the envelope coreinput forwards to its subscriber. Focus
    // is the top-most visible window; a key routes to its client (us); a title-bar
    // pointer press begins a compositor-internal MOVE. ---
    // a key with no visible window is dropped, not a crash.
    "  (send comp (list 'input (list 'key 1 1)))"
    "  (send h (list 'create-surface 40 40))"
    "  (define s4 (recv))"
    "  (define w4 (cadr s4))"
    "  (send h (list 'configure w4 10 10 #t))"            // place visible at (10,10)
    "  (send h (list 'commit w4 0 '()))"
    // SYNC: configure/commit are relayed via the handler, but (input ev) goes
    // DIRECT to the root and could overtake them (window not yet visible). A
    // relayed probe, recv'd, drains the handler -> root FIFO first, so the window
    // is visible before any input below. (The same barrier guards the in-OS test.)
    "  (send h (list 'probe-pixel 0 0))"
    "  (recv)"
    // keyboard -> the focused window's client (us), verbatim.
    "  (send comp (list 'input (list 'key 7 1)))"
    "  (define ke (recv))"
    "  (ck 'input-key-tag  (car ke)  'input)"
    "  (ck 'input-key-body (cadr ke) (list 'key 7 1))"
    // a malformed input event falls through harmlessly; the next key still routes.
    "  (send comp (list 'input 'garbage))"
    "  (send comp (list 'input (list 'key 8 1)))"
    "  (ck 'input-after-bad (cadr (recv)) (list 'key 8 1))"
    // title-bar drag: press in the title strip (y-10 < TITLE-H), motion, release ->
    // the window moves (10,10) -> (30,30) [grab offset (5,2); release at (35,32)].
    "  (send comp (list 'input (list 'pointer 15 12 #t)))" // press in title
    "  (send comp (list 'input (list 'pointer 35 32 #t)))" // drag
    "  (send comp (list 'input (list 'pointer 35 32 #f)))" // release
    // probe via the handler so it is FIFO-ordered AFTER the moves (the press/move/
    // release are already enqueued on the root). The fake backing is zeroed, so the
    // window reads 0 where it now is; the vacated old spot recomposites to background.
    "  (send h (list 'probe-pixel 15 12))"                // old title spot -> background
    "  (ck 'move-vacated (recv) (rgb screen 28 30 44))"
    "  (send h (list 'probe-pixel 35 32))"                // new title spot -> window (zeroed)
    "  (ck 'move-landed (recv) 0)"
    // --- 4b. drag-state is released when the dragged surface is destroyed
    // mid-drag (else the root's drag logic pins on the stale id and no further
    // window is draggable). Start a drag on w4, destroy it, then a fresh window
    // must still be draggable. (w4 is at (30,30) after the move above.) ---
    "  (send comp (list 'input (list 'pointer 33 32 #t)))" // press w4 title -> drag starts
    "  (send h (list 'destroy-surface w4))"                // destroy mid-drag -> clears drag
    "  (ck 'destroy-dragged-ok (recv) 'ok)"
    "  (send h (list 'create-surface 20 20))"
    "  (define w5 (cadr (recv)))"
    "  (send h (list 'configure w5 5 5 #t))"
    "  (send h (list 'commit w5 0 '()))"
    "  (send h (list 'probe-pixel 0 0)) (recv)"            // sync barrier (drain relay)
    "  (send comp (list 'input (list 'pointer 8 6 #t)))"   // press w5 title (local 3,1) -> NEW drag
    "  (send comp (list 'input (list 'pointer 18 16 #t)))" // move -> w5 to (15,15)
    "  (send comp (list 'input (list 'pointer 18 16 #f)))" // release
    "  (send h (list 'probe-pixel 8 6))"                   // vacated -> background
    "  (ck 'wedge-vacated (recv) (rgb screen 28 30 44))"
    "  (send h (list 'probe-pixel 18 16))"                 // landed -> window (zeroed)
    "  (ck 'wedge-landed (recv) 0)"
    "  (if (null? fails) 'ALLOK (cons 'FAILED (reverse fails))))))";

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    snprintf(g_lispdir, sizeof g_lispdir, "%s/../../../lisp", dir);

    uintptr_t stack_marker;
    lisp_gc_init(&stack_marker);
    lisp_value env = lisp_default_env();
    lisp_install_sched(env);
    lisp_set_module_loader(comp_loader, NULL);

    printf("[lisp compositor] paint-windows + surface protocol (phases 2-3)\n");

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
        printf("  ok   painter occludes/positions correctly; connect + per-client\n");
        printf("       handler relay + create/configure/commit/destroy survive\n");
        return 0;
    }
    printf("  FAIL state=%d err=%s\n  result: %s\n", lisp_ctx_state(t),
           err ? err : "(none)", buf);
    return 1;
}
