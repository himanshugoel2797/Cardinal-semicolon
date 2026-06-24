// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// Host-built test for K3: per-context garbage collection. When the scheduler runs
// with per-context heaps, each green-thread context owns its own heap, collected
// PRECISELY (from its CEK registers) and INDEPENDENTLY (pausing only that context)
// at safe points. The shared system heap holds the interned symbols and the global
// environment (an external, read-only region for a context's collector), and
// messages are deep-copied into the receiver's heap (shared-nothing).
//
// It checks:
//   1. Two contexts each churn a lot of garbage; each one's OWN heap is collected
//      during its run and its live set stays bounded, while both still produce the
//      right result (one context's collection does not disturb the other's data).
//   2. A message deep-copied into a receiver survives many collections of the
//      receiver's own heap while it is held, and arrives structurally intact --
//      proving cross-context data lives in the receiver's heap and is a precise
//      root there.

#include <stdio.h>
#include <string.h>

#include "lisp.h"

static int failures = 0;
static int checks = 0;

static void check_str(const char *label, const char *got, const char *want) {
    checks++;
    if (strcmp(got, want) != 0) {
        printf("  FAIL %-44s got '%s' want '%s'\n", label, got, want);
        failures++;
    } else {
        printf("  ok   %-44s -> %s\n", label, got);
    }
}

static void check_true(const char *label, int cond) {
    checks++;
    if (!cond) {
        printf("  FAIL %-44s (expected true)\n", label);
        failures++;
    } else {
        printf("  ok   %-44s\n", label);
    }
}

static const char *render(lisp_value v, char *buf, size_t cap) {
    lisp_print(v, buf, cap);
    return buf;
}

int main(void) {
    uintptr_t stack_marker;
    lisp_gc_init(&stack_marker);

    printf("[lisp ctxgc] per-context GC: independent heaps + shared-immutable region\n");

    // 1. Two garbage-churning contexts, each with its own heap. Each counts to a
    //    bound (allocating a fresh frame/env per iteration), so each heap must be
    //    collected during the run and stay bounded -- independently.
    {
        lisp_sched_t s;
        lisp_sched_init(&s, 200);
        s.per_context_heaps = 1;
        lisp_value env = lisp_default_env();
        lisp_install_sched(env);
        const char *err = NULL;
        char buf[64];

        lisp_value a = lisp_eval_string(
            "(spawn (lambda () (let loop ((i 0) (n 100000)) (if (= n 0) i (begin (cons i n) (loop (+ i 1) (- n 1)))))))",
            env, &err);
        lisp_value b = lisp_eval_string(
            "(spawn (lambda () (let loop ((i 0) (n 150000)) (if (= n 0) i (begin (cons i n) (loop (+ i 2) (- n 1)))))))",
            env, &err);

        lisp_sched_run(&s, 0);

        check_str("ctx A result", render(lisp_ctx_value(a), buf, sizeof buf), "100000");
        check_str("ctx B result", render(lisp_ctx_value(b), buf, sizeof buf), "300000");
        check_true("ctx A heap was collected", lisp_ctx_heap_collections(a) > 0);
        check_true("ctx B heap was collected", lisp_ctx_heap_collections(b) > 0);
        // Each heap reclaimed its per-iteration garbage: live is a tiny working set,
        // nowhere near the >100k iterations' worth of frames/envs allocated.
        check_true("ctx A live set bounded", lisp_ctx_heap_live(a) < 20000);
        check_true("ctx B live set bounded", lisp_ctx_heap_live(b) < 20000);
    }

    // 2. A message survives the receiver's own collections and arrives intact. The
    //    consumer receives a structured value, then holds it across a long loop
    //    that forces several collections of ITS heap; the message (deep-copied into
    //    the consumer's heap, rooted via its environment) must survive unchanged.
    {
        lisp_sched_t s;
        lisp_sched_init(&s, 150);
        s.per_context_heaps = 1;
        lisp_value env = lisp_default_env();
        lisp_install_sched(env);
        const char *err = NULL;
        char buf[64];

        lisp_eval_string(
            "(define consumer"
            "  (spawn (lambda ()"
            "           (let ((msg (recv)))"
            "             (let loop ((n 120000)) (if (= n 0) msg (begin (cons n n) (loop (- n 1)))))))))"
            "(define producer"
            "  (spawn (lambda () (send consumer (list 1 2 3 \"hi\" (vector 9 8))))))",
            env, &err);
        lisp_value consumer = lisp_eval_string("consumer", env, &err);

        lisp_sched_run(&s, 0);

        check_str("message intact after receiver GC",
                  render(lisp_ctx_value(consumer), buf, sizeof buf), "(1 2 3 \"hi\" #(9 8))");
        check_true("receiver heap was collected while holding it",
                   lisp_ctx_heap_collections(consumer) > 0);
    }

    printf("\n[lisp ctxgc] %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
