// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// Host test for the sys-debug reflective capability (debug.c): single-stepping a
// context with ctx-make/ctx-step and reading its registers, plus the proof that
// sys-debug is gated like any other capability module.

#include <stdio.h>
#include <string.h>

#include "lisp.h"

static int checks = 0;
static int failures = 0;

// Evaluate `src` in a fresh env with sys-debug registered (root authority, so the
// import is allowed) and check the printed result.
static void deval(const char *src, const char *expect) {
    checks++;
    lisp_value env = lisp_default_env();
    lisp_register_debug_module(env);
    const char *err = NULL;
    lisp_value v = lisp_eval_string(src, env, &err);
    if (v == LISP_UNDEF && err != NULL) {
        printf("  FAIL %-52s -> error: %s\n", src, err);
        failures++;
        return;
    }
    char buf[256];
    lisp_print(v, buf, sizeof(buf));
    if (strcmp(buf, expect) != 0) {
        printf("  FAIL %-52s -> got '%s' want '%s'\n", src, buf, expect);
        failures++;
    } else {
        printf("  ok   %-52s -> %s\n", src, buf);
    }
}

// Evaluate expecting an error whose message contains `needle`.
static void derr(const char *src, const char *needle) {
    checks++;
    lisp_value env = lisp_default_env();
    lisp_register_debug_module(env);
    const char *err = NULL;
    lisp_value v = lisp_eval_string(src, env, &err);
    if (v != LISP_UNDEF || err == NULL || strstr(err, needle) == NULL) {
        printf("  FAIL %-52s -> expected error '%s', got '%s'\n", src, needle,
               err ? err : "(none)");
        failures++;
    } else {
        printf("  ok   %-52s -> error: %s\n", src, err);
    }
}

static void ctx_done(lisp_value ctx, const char *expect, const char *label) {
    checks++;
    if (lisp_ctx_state(ctx) != LISP_CTX_DONE) {
        printf("  FAIL %-46s -> not DONE (state %d) err=%s\n", label,
               lisp_ctx_state(ctx),
               lisp_ctx_error(ctx) ? lisp_ctx_error(ctx) : "(none)");
        failures++;
        return;
    }
    char buf[64];
    lisp_print(lisp_ctx_value(ctx), buf, sizeof(buf));
    if (strcmp(buf, expect) != 0) {
        printf("  FAIL %-46s -> got '%s' want '%s'\n", label, buf, expect);
        failures++;
    } else {
        printf("  ok   %-46s -> %s\n", label, buf);
    }
}

static void ctx_errs(lisp_value ctx, const char *needle, const char *label) {
    checks++;
    const char *m = lisp_ctx_error(ctx);
    if (lisp_ctx_state(ctx) != LISP_CTX_ERROR || m == NULL || strstr(m, needle) == NULL) {
        printf("  FAIL %-46s -> state %d err '%s' want '%s'\n", label,
               lisp_ctx_state(ctx), m ? m : "(none)", needle);
        failures++;
    } else {
        printf("  ok   %-46s -> error: %s\n", label, m);
    }
}

int main(void) {
    uintptr_t stack_marker;
    lisp_gc_init(&stack_marker);

    printf("[lisp sys-debug] single-step + inspect\n");

    // Step a computation to completion: result is right, and it took >1 step
    // (proving ctx-step really advances one reduction at a time, not all at once).
    deval("(import sys-debug)"
          "(define c (ctx-make (lambda () (+ 1 (* 2 3)))))"
          "(let loop ((k 0))"
          "  (if (eq? (ctx-step c) 'done)"
          "      (list (ctx-value c) (> k 0) (ctx-status c))"
          "      (loop (+ k 1))))",
          "(7 #t done)");

    // The stepped thunk sees its surrounding lexical scope (closure captures env).
    deval("(import sys-debug)"
          "(define c (let ((x 40)) (ctx-make (lambda () (+ x 2)))))"
          "(let loop () (if (eq? (ctx-step c) 'done) (ctx-value c) (loop)))",
          "42");

    // A fresh context is in the EVAL state with its control set (an application).
    deval("(import sys-debug)"
          "(define c (ctx-make (lambda () (+ 1 2))))"
          "(list (ctx-status c) (pair? (ctx-control c)))",
          "(eval #t)");

    // An error in the stepped code surfaces as status 'error + a message string.
    deval("(import sys-debug)"
          "(define c (ctx-make (lambda () (car 5))))"
          "(let loop ()"
          "  (let ((s (ctx-step c)))"
          "    (cond ((eq? s 'error) (list s (string? (ctx-error c))))"
          "          ((eq? s 'done) 'unexpected-done)"
          "          (else (loop)))))",
          "(error #t)");

    // ctx-value of an unfinished context is unspecified; ctx-error is #f.
    deval("(import sys-debug) (ctx-error (ctx-make (lambda () 1)))", "#f");

    // Argument checking.
    derr("(import sys-debug) (ctx-make 5)", "procedure");
    derr("(import sys-debug) (ctx-step 5)", "context");
    derr("(import sys-debug) (ctx-status 5)", "context");
    derr("(import sys-debug) (ctx-step (ctx-make (lambda () 1)) 0)", "positive");

    printf("[lisp sys-debug] capability gating\n");
    {
        lisp_value env = lisp_default_env();
        lisp_install_sched(env);
        lisp_register_debug_module(env);
        lisp_sched_t s;
        lisp_sched_init(&s, 100000);
        s.per_context_heaps = 0;

        const char *err = NULL;
        lisp_eval_string(
            // a context with no grant cannot import the debugger...
            "(define denied (spawn-restricted '()"
            "  (lambda () (import sys-debug) 'leaked)))"
            // ...one granted sys-debug can, and can drive a sub-context.
            "(define ok (spawn-restricted '(sys-debug)"
            "  (lambda ()"
            "    (import sys-debug)"
            "    (let ((c (ctx-make (lambda () (* 6 7)))))"
            "      (let loop () (if (eq? (ctx-step c) 'done) (ctx-value c) (loop)))))))",
            env, &err);
        lisp_value denied = lisp_eval_string("denied", env, &err);
        lisp_value ok = lisp_eval_string("ok", env, &err);
        if (err != NULL) {
            printf("  FAIL gating setup -> %s\n", err);
            failures++;
        }
        lisp_sched_run(&s, 0);
        ctx_errs(denied, "capability not granted", "sys-debug import denied without grant");
        ctx_done(ok, "42", "sys-debug import + single-step works with grant");
    }

    // Regression: step a sub-context from inside a SCHEDULED context that has its
    // own per-context heap (the production config). The sub-context allocates a
    // 10000-cons list (>256KB GC threshold), so its heap collects mid-step. If the
    // stepped context lacked its own heap it would allocate into the driver's heap
    // and be swept there -> a cross-heap use-after-free. Correct result == 10000.
    printf("[lisp sys-debug] stepping under per-context-heap GC\n");
    {
        lisp_value env = lisp_default_env();
        lisp_install_sched(env);
        lisp_register_debug_module(env);
        lisp_sched_t s;
        lisp_sched_init(&s, 1000000);
        s.per_context_heaps = 1;
        const char *err = NULL;
        lisp_eval_string(
            "(define worker (spawn-restricted '(sys-debug)"
            "  (lambda ()"
            "    (import sys-debug)"
            "    (let ((c (ctx-make (lambda ()"
            "                (let loop ((i 0) (acc '()))"
            "                  (if (= i 10000) (length acc)"
            "                      (loop (+ i 1) (cons i acc))))))))"
            "      (let drive () (if (eq? (ctx-step c 16) 'done) (ctx-value c) (drive)))))))",
            env, &err);
        lisp_value worker = lisp_eval_string("worker", env, &err);
        if (err != NULL) {
            printf("  FAIL stress setup -> %s\n", err);
            failures++;
        }
        lisp_sched_run(&s, 0);
        ctx_done(worker, "10000", "step a 10000-alloc sub-context, per-ctx-heap GC");
    }

    printf("\n[lisp sys-debug] %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
