// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// Host test for the finalizer-bearing GC handle (LISP_OBJ_HANDLE). A handle owns
// an opaque C resource and runs a finalizer EXACTLY ONCE when the GC reclaims it
// -- in the sweep path of its heap, or at heap teardown (lisp_heap_free). It is
// identity-only and non-transferable: (send)ing one is rejected, because copying
// or aliasing it would let two heaps finalize the same resource (double free).
//
// Coverage:
//   (a) reclaimed in a system-heap collection -> finalizer ran 0->1 (exactly once)
//   (b) live in a context heap torn down via lisp_heap_free -> finalizer ran
//   (c) lisp_handle_clear before drop -> finalizer does NOT run
//   (d) (send) of a handle is rejected; the finalizer is not double-invoked
//   (e) tag round-trips via lisp_handle_tag
//
// (b) needs to allocate into a specific (context) heap, which is an internal API,
// so this test reaches into ../src/internal.h -- the harness compiles the lib
// sources alongside it, so the symbols link.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lisp.h"

#include "../src/internal.h"  // lisp_gc_set_alloc_heap / lisp_gc_system_heap

static int failures = 0;
static int checks = 0;

static void chk(const char *what, int cond) {
    checks++;
    if (!cond) {
        printf("  FAIL %s\n", what);
        failures++;
    } else {
        printf("  ok   %s\n", what);
    }
}

// A finalizer that bumps a counter, so a test can prove it ran exactly once.
static int g_fin_calls = 0;
static void counting_fin(void *p) {
    (void)p;
    g_fin_calls++;
}

// A finalizer that frees its malloc'd resource -- the realistic shape (a Wasm
// instance). ASan/UBSan catch a leak (never finalized) or a double free (twice).
static void free_fin(void *p) { free(p); }

static const char *render(lisp_value v, char *buf, size_t cap) {
    lisp_print(v, buf, cap);
    return buf;
}

int main(void) {
    uintptr_t stack_marker;
    lisp_gc_init(&stack_marker);
    lisp_default_env();  // builds the system heap / interns

    printf("[lisp handle] finalize-on-GC / teardown / clear / send-reject / tag\n");

    // (a) A handle dropped before a collection is reclaimed and finalized exactly
    //     once. Allocate it WITHOUT keeping a C-local root (so the conservative
    //     scan can't see it), force two collections, and check the counter.
    {
        g_fin_calls = 0;
        // No surviving reference: the value is consumed by lisp_handle_tag and not
        // stored, so after this statement nothing roots the handle.
        chk("fresh handle has the right tag",
            lisp_handle_tag(lisp_make_handle((void *)0x1, counting_fin, 0xABCD)) == 0xABCD);
        // A real free_fin handle whose backing is a malloc -- ASan flags a leak if
        // it is never finalized.
        lisp_make_handle(malloc(32), free_fin, 1);
        lisp_gc_collect();
        lisp_gc_collect();
        chk("dropped handle's finalizer ran exactly once (0->1)", g_fin_calls == 1);
    }

    // A live (rooted) handle is NOT finalized while reachable, then IS once dropped.
    {
        g_fin_calls = 0;
        volatile lisp_value keep = lisp_make_handle((void *)0x2, counting_fin, 7);
        lisp_gc_collect();
        lisp_gc_collect();
        chk("rooted handle is not finalized", g_fin_calls == 0);
        chk("rooted handle still reports its tag", lisp_handle_tag((lisp_value)keep) == 7);
        keep = LISP_UNDEF;  // drop the only reference
        lisp_gc_collect();
        lisp_gc_collect();
        chk("handle finalized once after the reference is dropped", g_fin_calls == 1);
    }

    // (b) A handle living in a CONTEXT heap is finalized when that heap is torn
    //     down (lisp_heap_free), even though it was never swept. Allocate it into a
    //     fresh heap, then free the heap.
    {
        g_fin_calls = 0;
        // A heap with a real owning context: lisp_gc_alloc routes allocations to a
        // per-context heap only when its owner != LISP_EMPTY (an LISP_EMPTY owner
        // means "the system heap"). The ctx need not run; it just owns the heap.
        lisp_value owner = lisp_ctx_make(LISP_EMPTY, LISP_EMPTY);
        lisp_heap_t *h = lisp_heap_new(owner);
        chk("context heap allocated", h != NULL);
        lisp_heap_t *prev = lisp_gc_set_alloc_heap(h);
        lisp_value hv = lisp_make_handle(malloc(16), free_fin, 2);
        lisp_value hv2 = lisp_make_handle((void *)0x3, counting_fin, 3);
        (void)hv;
        (void)hv2;
        lisp_gc_set_alloc_heap(prev);
        chk("nothing finalized before teardown", g_fin_calls == 0);
        lisp_heap_free(h);  // tears down the heap -> finalizes its live handles
        chk("teardown finalized the counting handle once", g_fin_calls == 1);
        // (free_fin handle's malloc is freed by lisp_heap_free's gc_finalize;
        //  ASan would flag a leak or double free.)
    }

    // (c) lisp_handle_clear disarms a handle: a subsequent collection must NOT run
    //     its finalizer (the explicit-destroy path already released the resource).
    {
        g_fin_calls = 0;
        lisp_value hv = lisp_make_handle((void *)0x4, counting_fin, 9);
        lisp_handle_clear(hv);
        chk("cleared handle's ptr is NULL", lisp_handle_ptr(hv) == NULL);
        hv = LISP_UNDEF;  // drop it
        lisp_gc_collect();
        lisp_gc_collect();
        chk("cleared handle's finalizer did NOT run", g_fin_calls == 0);
    }

    // (d) (send) of a handle is rejected, and the rejection must not finalize it
    //     (no double free of the still-owned resource).
    {
        g_fin_calls = 0;
        lisp_sched_t s;
        lisp_sched_init(&s, 64);
        lisp_value env = lisp_default_env();
        lisp_install_sched(env);
        const char *err = NULL;
        char buf[96];

        // Keep the handle rooted through the env binding so the GC can't reclaim it
        // during the run (we want to observe that SEND, not GC, leaves it alone).
        lisp_env_define(env, lisp_make_symbol("hv", 2),
                        lisp_make_handle((void *)0x5, counting_fin, 11));
        lisp_eval_string(
            "(define consumer (spawn (lambda () (recv))))"
            "(define producer (spawn (lambda () (send consumer hv))))",
            env, &err);
        if (err)
            printf("  (send setup error: %s)\n", err);
        lisp_value producer = lisp_eval_string("producer", env, &err);
        lisp_sched_run(&s, 0);

        chk("producer ERRORed trying to send a handle",
            lisp_ctx_state(producer) == LISP_CTX_ERROR);
        const char *perr = lisp_ctx_error(producer);
        chk("error is the handle-transfer rejection",
            perr != NULL && strstr(perr, "cannot transfer a handle") != NULL);
        chk("rejected send did NOT finalize the handle", g_fin_calls == 0);

        // The original binding still names a usable handle (it was not consumed).
        lisp_value tag = lisp_eval_string("hv", env, &err);
        chk("handle still intact after a rejected send", lisp_handle_tag(tag) == 11);
        (void)render(tag, buf, sizeof buf);
    }

    // (e) tag round-trip (also: a NULL-finalizer handle is reclaimed without a
    //     crash, and accessors on a non-handle are benign).
    {
        lisp_value h0 = lisp_make_handle((void *)0xDEAD, NULL, 0xFEEDFACE);
        chk("tag round-trips", lisp_handle_tag(h0) == 0xFEEDFACE);
        chk("ptr round-trips", lisp_handle_ptr(h0) == (void *)0xDEAD);
        chk("is_handle is true for a handle", lisp_is_handle(h0));
        chk("is_handle is false for a fixnum", !lisp_is_handle(lisp_fixnum(5)));
        chk("ptr of a non-handle is NULL", lisp_handle_ptr(lisp_fixnum(5)) == NULL);
        chk("tag of a non-handle is 0", lisp_handle_tag(lisp_fixnum(5)) == 0);
        h0 = LISP_UNDEF;
        lisp_gc_collect();  // a NULL-finalizer handle reclaims without incident
    }

    printf("[lisp handle] %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
