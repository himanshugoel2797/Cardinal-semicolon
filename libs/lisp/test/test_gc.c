// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// Host-built GC test: conservative mark-sweep. Verifies that (a) reachable data
// survives collection, (b) garbage is actually reclaimed (live count stays
// bounded under heavy short-lived allocation), and (c) nested structures of
// every object type survive.

#include <stdio.h>
#include <string.h>

#include "lisp.h"

static int failures = 0;
static int checks = 0;

static void expect(const char *label, const char *got, const char *want) {
    checks++;
    if (strcmp(got, want) != 0) {
        printf("  FAIL %-36s got '%s' want '%s'\n", label, got, want);
        failures++;
    } else {
        printf("  ok   %-36s -> %s\n", label, got);
    }
}

static void check_true(const char *label, int cond) {
    checks++;
    if (cond) {
        printf("  ok   %-36s\n", label);
    } else {
        printf("  FAIL %-36s\n", label);
        failures++;
    }
}

// Foreign-handle finalizer probe: the GC calls this once when a handle dies.
static int g_fin_count = 0;
static void *g_fin_last = NULL;
static void count_finalizer(void *p) {
    g_fin_count++;
    g_fin_last = p;
}

// Mint a handle whose ONLY reference is this frame's local, then return -- so the
// handle is unreachable once the frame is gone. Kept out of line so the compiler
// cannot keep the value live in the caller (the harness builds at -O0, no inline).
static void mint_dead_handle(void *slot) {
    lisp_value h = lisp_make_handle(slot, count_finalizer, 0xBEEF);
    if (!lisp_is_handle(h))
        printf("  FAIL mint_dead_handle produced a non-handle\n");
}

// Allocate a heap of immediately-dead pairs to force a collection (and overwrite
// the stack slots the previous frame used, so a stale pointer can't pin a handle).
static void churn_garbage(void) {
    for (int i = 0; i < 200000; i++)
        (void)lisp_cons(lisp_fixnum(i), LISP_EMPTY);
}

int main(void) {
    uintptr_t stack_marker;
    lisp_gc_init(&stack_marker);  // enable GC; this frame is the stack base
    const char *err = NULL;
    char buf[128];

    printf("[lisp GC] conservative mark-sweep\n");

    // 1. A reachable, named structure survives a forced collection.
    {
        lisp_value env = lisp_default_env();
        lisp_eval_string("(define keep (list 1 2 3 4 5))", env, &err);
        lisp_gc_collect();
        lisp_gc_collect();
        lisp_value r = lisp_eval_string("(apply + keep)", env, &err);
        lisp_print(r, buf, sizeof(buf));
        expect("retained list survives GC", buf, "15");
    }

    // 2. Garbage is reclaimed: allocate ~300k short-lived pairs; live count must
    //    stay bounded (not grow ~300k), and GC must have run.
    {
        lisp_value env = lisp_default_env();
        size_t before = lisp_gc_live_count();
        size_t cols0 = lisp_gc_collections();
        lisp_eval_string(
            "(define (burn i) (if (= i 0) 'done (begin (cons i i) (burn (- i 1)))))"
            " (burn 300000)",
            env, &err);
        lisp_gc_collect();
        size_t after = lisp_gc_live_count();
        size_t cols = lisp_gc_collections();

        checks++;
        if (cols <= cols0) {
            printf("  FAIL GC never ran during heavy allocation\n");
            failures++;
        } else {
            printf("  ok   GC ran %zu times under allocation pressure\n", cols - cols0);
        }

        checks++;
        if (after > before + 20000) {
            printf("  FAIL garbage not reclaimed: live %zu -> %zu\n", before, after);
            failures++;
        } else {
            printf("  ok   garbage reclaimed (live %zu -> %zu, not ~300000)\n", before, after);
        }
    }

    // 3. Nested structures of every traced type survive repeated collection.
    {
        lisp_value env = lisp_default_env();
        lisp_eval_string(
            "(define tree (list (list 1 2) (vector 3 4) \"hi\" (cons 'a 'b) 2.5))",
            env, &err);
        lisp_gc_collect();
        lisp_gc_collect();
        // Probes a pair, a vector, a string, a dotted pair, and a flonum.
        lisp_value r = lisp_eval_string(
            "(list (car (car tree)) (vector-ref (cadr tree) 1)"
            "      (caddr tree) (cdr (cadddr tree)) (list-ref tree 4))",
            env, &err);
        lisp_print(r, buf, sizeof(buf));
        expect("nested structure survives GC", buf, "(1 4 \"hi\" b 2.5)");
    }

    // 4. A closure capturing its environment survives GC and still works.
    {
        lisp_value env = lisp_default_env();
        lisp_eval_string("(define (adder n) (lambda (x) (+ x n))) (define add7 (adder 7))",
                         env, &err);
        lisp_gc_collect();
        lisp_value r = lisp_eval_string("(add7 35)", env, &err);
        lisp_print(r, buf, sizeof(buf));
        expect("captured closure survives GC", buf, "42");
    }

    // 5. Foreign handles: accessors read back, the finalizer never runs while the
    //    handle is reachable, and runs exactly once when it dies. (The shader tier
    //    rides this to reclaim a compiled program with no manual free.)
    {
        g_fin_count = 0;
        g_fin_last = NULL;
        int slot_a = 0;
        // A reachable handle (held in a local the conservative scan sees).
        volatile lisp_value live = lisp_make_handle(&slot_a, count_finalizer, 0xC0DE);
        lisp_value lh = (lisp_value)live;
        check_true("is-handle predicate", lisp_is_handle(lh));
        check_true("handle ptr accessor", lisp_handle_ptr(lh) == &slot_a);
        check_true("handle tag accessor", lisp_handle_tag(lh) == 0xC0DE);
        check_true("non-handle predicate rejects a fixnum", !lisp_is_handle(lisp_fixnum(7)));
        churn_garbage();
        lisp_gc_collect();
        lisp_gc_collect();
        check_true("reachable handle NOT finalized", g_fin_count == 0);
        (void)live;  // keep it rooted across the collections above

        // A separate handle that becomes unreachable must be finalized exactly once.
        g_fin_count = 0;
        g_fin_last = NULL;
        int slot_b = 0;
        mint_dead_handle(&slot_b);  // its only reference dies with the helper frame
        churn_garbage();            // forces a collection; reclaims the dead handle
        lisp_gc_collect();
        lisp_gc_collect();
        check_true("dead handle finalized exactly once", g_fin_count == 1);
        check_true("finalizer received the wrapped pointer", g_fin_last == &slot_b);
    }

    printf("\n[lisp GC] %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
