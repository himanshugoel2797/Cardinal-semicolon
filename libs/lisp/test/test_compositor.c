// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// Host test for the compositor IPC skeleton (lisp/servers/corecompositor.clp,
// phase 2 of notes/servers/CoreCompositor.md). The whole of phase 2 is actor-model
// orchestration -- the well-known PRIMARY mailbox, the `connect` handshake, and the
// spawned PER-CLIENT HANDLER context (the "secondary channel") -- with no hardware
// in the loop, so a host run under the real scheduler covers it exactly. (Phase 3's
// surface ops need the kernel's grant/vmem prims and move to the in-OS self-test.)
//
// We drive a client context that:
//   - connects to the primary mailbox and gets back a distinct handler handle;
//   - pings the handler over the secondary channel and gets (pong) -- proving the
//     spawned context is alive and demuxes the tagged envelope;
//   - a SECOND client connects and gets a DIFFERENT handler -- proving per-client
//     isolation (each connection owns its own context/mailbox), and that the
//     primary mailbox stays free for handshakes (it never carried the ping).
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

// One client context runs the whole scenario synchronously by embedding (self) as
// the reply address and (recv)ing each reply. It accumulates failures into `fails`
// and returns 'ALLOK iff every check passed. `eq?` is the load-bearing assertion:
// the handler handle must arrive by identity, and the two clients' handlers must be
// distinct context objects.
static const char *PROG =
    "(import corecompositor)"
    "(define comp (start-compositor-service))"
    "(define t (spawn (lambda ()"
    "  (define fails '())"
    "  (define (ck name got want)"
    "    (if (equal? got want) #t (set! fails (cons (list name got want) fails))))"
    // --- handshake: connect (opaque) -> (connected handler) ---
    "  (send comp (list 'connect #f (self)))"
    "  (define r1 (recv))"
    "  (ck 'c1-tag (car r1) 'connected)"
    "  (define h1 (cadr r1))"
    // a distinct, live context: not the primary mailbox (eq? #f), and it answers
    // pings below -- together that is exactly "a real per-client handler".
    "  (ck 'c1-handler-not-primary (eq? h1 comp) #f)"
    // --- secondary channel: ping the handler, expect (pong) ---
    "  (send h1 (list 'ping (self)))"
    "  (ck 'c1-pong (car (recv)) 'pong)"
    // --- a second client gets its OWN handler (per-client isolation) ---
    "  (send comp (list 'connect #t (self)))"
    "  (define r2 (recv))"
    "  (ck 'c2-tag (car r2) 'connected)"
    "  (define h2 (cadr r2))"
    "  (ck 'c2-distinct-handler (eq? h1 h2) #f)"
    "  (send h2 (list 'ping (self)))"
    "  (ck 'c2-pong (car (recv)) 'pong)"
    // h1 still answers after h2 exists -> the two handlers are independent.
    "  (send h1 (list 'ping (self)))"
    "  (ck 'c1-pong-again (car (recv)) 'pong)"
    // a malformed (connect) (missing reply addr) must NOT crash the primary serve
    // context -- it is ignored, and a subsequent well-formed connect still works.
    "  (send comp (list 'connect))"
    "  (send comp (list 'connect #f (self)))"
    "  (ck 'survives-bad-connect (car (recv)) 'connected)"
    "  (if (null? fails) 'ALLOK (cons 'FAILED (reverse fails))))))";

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    snprintf(g_lispdir, sizeof g_lispdir, "%s/../../../lisp", dir);

    uintptr_t stack_marker;
    lisp_gc_init(&stack_marker);
    lisp_value env = lisp_default_env();
    lisp_install_sched(env);
    lisp_set_module_loader(comp_loader, NULL);

    printf("[lisp compositor] connect handshake + per-client handler (phase 2)\n");

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
        printf("  ok   connect -> distinct handler, ping/pong over the secondary\n");
        printf("       channel, two clients isolated on independent handlers\n");
        return 0;
    }
    printf("  FAIL state=%d err=%s\n  result: %s\n", lisp_ctx_state(t),
           err ? err : "(none)", buf);
    return 1;
}
