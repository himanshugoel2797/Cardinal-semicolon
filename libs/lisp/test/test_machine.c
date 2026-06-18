// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// Host-built test for the explicit-stack CEK machine (eval.c). It exercises the
// two capabilities the old C-stack tree-walker could not express, and which the
// process model is built on:
//
//   1. Deep NON-tail recursion runs on a heap-resident continuation chain, not
//      the C stack -- so it does not overflow at depths that would blow a native
//      stack.
//   2. A computation can be SUSPENDED at a safe point (when a per-slice reduction
//      budget runs out) and RESUMED later, deterministically reaching the same
//      result -- and its state survives a garbage collection performed while it
//      is parked (the context is a precise GC root).
//
// It also checks that the synchronous wrappers still give tail calls O(1) frames
// (a 1M-iteration loop completes) and that errors surface as an ERROR status.

#include <stdio.h>
#include <string.h>

#include "lisp.h"

static int failures = 0;
static int checks = 0;

static void check_str(const char *label, const char *got, const char *want) {
    checks++;
    if (strcmp(got, want) != 0) {
        printf("  FAIL %-38s got '%s' want '%s'\n", label, got, want);
        failures++;
    } else {
        printf("  ok   %-38s -> %s\n", label, got);
    }
}

static void check_true(const char *label, int cond) {
    checks++;
    if (!cond) {
        printf("  FAIL %-38s (expected true)\n", label);
        failures++;
    } else {
        printf("  ok   %-38s\n", label);
    }
}

// Evaluate `src` in `env` (synchronous wrapper) and render the result.
static const char *eval_to(lisp_value env, const char *src, char *buf, size_t cap) {
    const char *err = NULL;
    lisp_value r = lisp_eval_string(src, env, &err);
    if (err != NULL) {
        snprintf(buf, cap, "<error: %s>", err);
        return buf;
    }
    lisp_print(r, buf, cap);
    return buf;
}

// Read one datum from a source string (for building a context's initial expr).
static lisp_value read1(const char *src) {
    const char *cur = src;
    const char *end = src + strlen(src);
    const char *err = NULL;
    return lisp_read(&cur, end, &err);
}

int main(void) {
    uintptr_t stack_marker;
    lisp_gc_init(&stack_marker);  // the machine allocates per reduction; GC on

    printf("[lisp machine] explicit-stack CEK: deep recursion + suspend/resume\n");

    // 1. Deep NON-tail recursion. Each pending (+ n ...) keeps a live continuation
    //    frame, so this holds O(n) frames on the heap -- impossible on the C stack
    //    at this depth. sum 1..100000 = 5000050000.
    {
        lisp_value env = lisp_default_env();
        char buf[64];
        eval_to(env, "(define (sum n) (if (= n 0) 0 (+ n (sum (- n 1)))))", buf, sizeof buf);
        eval_to(env, "(sum 100000)", buf, sizeof buf);
        check_str("deep non-tail (sum 100000)", buf, "5000050000");
    }

    // 2. Suspend/resume under a small reduction budget, collecting WHILE suspended.
    //    The named let counts down 100000 iterations; driving it 1000 reductions
    //    at a time must yield SUSPENDED repeatedly and then DONE with 100000, and
    //    a GC between slices must not disturb the parked state.
    {
        lisp_value env = lisp_default_env();
        lisp_value expr = read1(
            "(let loop ((i 100000) (acc 0)) (if (= i 0) acc (loop (- i 1) (+ acc 1))))");
        lisp_value ctx = lisp_ctx_make(expr, env);
        int suspensions = 0;
        lisp_ctx_status st;
        while ((st = lisp_ctx_resume(ctx, 1000)) == LISP_CTX_SUSPENDED) {
            suspensions++;
            lisp_gc_collect();  // park is at a safe point: state must survive
        }
        char buf[64];
        lisp_print(lisp_ctx_value(ctx), buf, sizeof buf);
        check_str("suspend/resume result", buf, "100000");
        check_true("suspended at least once", suspensions > 0);
        check_true("finished DONE (not ERROR)", st == LISP_CTX_DONE);
    }

    // 3. Two independent contexts interleave on different budgets and each reaches
    //    its own correct result -- the green-thread shape the scheduler will use.
    {
        lisp_value env = lisp_default_env();
        lisp_value a = lisp_ctx_make(
            read1("(let loop ((i 50000) (s 0)) (if (= i 0) s (loop (- i 1) (+ s 1))))"), env);
        lisp_value b = lisp_ctx_make(
            read1("(let loop ((i 30000) (s 0)) (if (= i 0) s (loop (- i 1) (+ s 2))))"), env);
        lisp_ctx_status sa = LISP_CTX_SUSPENDED, sb = LISP_CTX_SUSPENDED;
        while (sa == LISP_CTX_SUSPENDED || sb == LISP_CTX_SUSPENDED) {
            if (sa == LISP_CTX_SUSPENDED)
                sa = lisp_ctx_resume(a, 777);
            if (sb == LISP_CTX_SUSPENDED)
                sb = lisp_ctx_resume(b, 555);
        }
        char ba[64], bb[64];
        lisp_print(lisp_ctx_value(a), ba, sizeof ba);
        lisp_print(lisp_ctx_value(b), bb, sizeof bb);
        check_str("interleaved ctx A (count 50000)", ba, "50000");
        check_str("interleaved ctx B (count*2 30000)", bb, "60000");
    }

    // 4. Tail recursion stays O(1) frames: a 1M-iteration loop completes via the
    //    synchronous wrapper (the analogue of the old `goto tail`).
    {
        lisp_value env = lisp_default_env();
        char buf[64];
        eval_to(env, "(define (loop n) (if (= n 0) 'done (loop (- n 1))))", buf, sizeof buf);
        eval_to(env, "(loop 1000000)", buf, sizeof buf);
        check_str("tail loop 1M completes", buf, "done");
    }

    // 5. An error surfaces as an ERROR status with a message, not a crash.
    {
        lisp_value env = lisp_default_env();
        lisp_value ctx = lisp_ctx_make(read1("(+ 1 no-such-variable)"), env);
        lisp_ctx_status st = lisp_ctx_resume(ctx, 1000000);
        check_true("error -> LISP_CTX_ERROR", st == LISP_CTX_ERROR);
        check_true("error message is set", lisp_ctx_error(ctx) != NULL);
    }

    // 6. A user (error "msg") message must remain valid after a collection while
    //    the context sits in ERROR state (the message must not point into the GC
    //    heap). Force allocation + collection between the error and reading it.
    {
        lisp_value env = lisp_default_env();
        lisp_value ctx = lisp_ctx_make(read1("(error \"custom-boom\")"), env);
        lisp_ctx_status st = lisp_ctx_resume(ctx, 1000000);
        // Churn the heap and collect: a dangling heap pointer would be corrupted.
        lisp_value scratch = lisp_default_env();
        (void)scratch;
        lisp_gc_collect();
        const char *msg = lisp_ctx_error(ctx);
        check_true("user error stays ERROR", st == LISP_CTX_ERROR);
        check_str("user error message survives GC", msg ? msg : "(null)", "custom-boom");
    }

    printf("\n[lisp machine] %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
