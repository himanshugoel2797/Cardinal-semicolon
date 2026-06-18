// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// Host-built test for the K2 cooperative scheduler (sched.c): contexts as green
// threads scheduled round-robin over reduction slices. It checks the three
// properties the process model needs:
//
//   1. Several contexts interleave under a budget and each reaches its own
//      correct result (no context monopolizes the scheduler).
//   2. Contexts communicate by copy-on-send messaging: a producer SENDs values to
//      a consumer that blocks in (recv) until they arrive, then drains them.
//   3. An infinite-loop context is preempted at safe points, so it does NOT wedge
//      the scheduler -- a sibling that terminates still finishes.

#include <stdio.h>
#include <string.h>

#include "lisp.h"

static int failures = 0;
static int checks = 0;

static void check_str(const char *label, const char *got, const char *want) {
    checks++;
    if (strcmp(got, want) != 0) {
        printf("  FAIL %-40s got '%s' want '%s'\n", label, got, want);
        failures++;
    } else {
        printf("  ok   %-40s -> %s\n", label, got);
    }
}

static void check_true(const char *label, int cond) {
    checks++;
    if (!cond) {
        printf("  FAIL %-40s (expected true)\n", label);
        failures++;
    } else {
        printf("  ok   %-40s\n", label);
    }
}

static const char *render(lisp_value v, char *buf, size_t cap) {
    lisp_print(v, buf, cap);
    return buf;
}

int main(void) {
    uintptr_t stack_marker;
    lisp_gc_init(&stack_marker);  // the machine allocates per reduction; GC on

    printf("[lisp sched] cooperative scheduler: interleave + messaging + preempt\n");

    // 1. Three counting contexts interleave under a small slice. Each counts up
    //    to a different bound; all must finish with the right value, and it must
    //    take many passes (proof that no context ran to completion in one slice).
    {
        lisp_sched_t s;
        lisp_sched_init(&s, 100);
        lisp_value env = lisp_default_env();
        lisp_install_sched(env);
        const char *err = NULL;
        char buf[64];

        lisp_value a = lisp_eval_string(
            "(spawn (lambda () (let loop ((i 0) (n 1000)) (if (= n 0) i (loop (+ i 1) (- n 1))))))",
            env, &err);
        lisp_value b = lisp_eval_string(
            "(spawn (lambda () (let loop ((i 0) (n 2000)) (if (= n 0) i (loop (+ i 1) (- n 1))))))",
            env, &err);
        lisp_value c = lisp_eval_string(
            "(spawn (lambda () (let loop ((i 0) (n 3000)) (if (= n 0) i (loop (+ i 1) (- n 1))))))",
            env, &err);

        int passes = lisp_sched_run(&s, 0);

        check_str("ctx A result (count 1000)", render(lisp_ctx_value(a), buf, sizeof buf), "1000");
        check_str("ctx B result (count 2000)", render(lisp_ctx_value(b), buf, sizeof buf), "2000");
        check_str("ctx C result (count 3000)", render(lisp_ctx_value(c), buf, sizeof buf), "3000");
        check_true("all three finished DONE", lisp_ctx_state(a) == LISP_CTX_DONE &&
                                                  lisp_ctx_state(b) == LISP_CTX_DONE &&
                                                  lisp_ctx_state(c) == LISP_CTX_DONE);
        check_true("interleaved over many passes", passes > 10);
    }

    // 2. Producer/consumer over copy-on-send. The consumer is spawned first, so it
    //    runs first and BLOCKS in (recv) with an empty mailbox; the producer then
    //    sends 1..5, waking it; the consumer drains and sums them = 15.
    {
        lisp_sched_t s;
        lisp_sched_init(&s, 64);
        lisp_value env = lisp_default_env();
        lisp_install_sched(env);
        const char *err = NULL;
        char buf[64];

        lisp_eval_string(
            "(define consumer"
            "  (spawn (lambda () (let loop ((acc 0) (k 5))"
            "                      (if (= k 0) acc (loop (+ acc (recv)) (- k 1)))))))"
            "(define producer"
            "  (spawn (lambda () (let loop ((i 1))"
            "                      (if (> i 5) 'done (begin (send consumer i) (loop (+ i 1))))))))",
            env, &err);
        lisp_value consumer = lisp_eval_string("consumer", env, &err);

        lisp_sched_run(&s, 0);

        check_str("consumer summed 1..5 via recv", render(lisp_ctx_value(consumer), buf, sizeof buf),
                  "15");
        check_true("consumer finished DONE", lisp_ctx_state(consumer) == LISP_CTX_DONE);
    }

    // 3. Copy-on-send delivers a structured (immutable) message intact.
    {
        lisp_sched_t s;
        lisp_sched_init(&s, 64);
        lisp_value env = lisp_default_env();
        lisp_install_sched(env);
        const char *err = NULL;
        char buf[64];

        lisp_eval_string(
            "(define sink (spawn (lambda () (recv))))"
            "(define src  (spawn (lambda () (send sink (list 1 \"two\" 'three (vector 4 5))))))",
            env, &err);
        lisp_value sink = lisp_eval_string("sink", env, &err);

        lisp_sched_run(&s, 0);

        check_str("structured message copied intact",
                  render(lisp_ctx_value(sink), buf, sizeof buf), "(1 \"two\" three #(4 5))");
    }

    // 4. An infinite-loop context does not wedge the scheduler: cap the passes and
    //    confirm a terminating sibling still finished while the spinner is merely
    //    suspended (preempted), not done.
    {
        lisp_sched_t s;
        lisp_sched_init(&s, 100);
        lisp_value env = lisp_default_env();
        lisp_install_sched(env);
        const char *err = NULL;
        char buf[64];

        lisp_value finite = lisp_eval_string(
            "(spawn (lambda () (let loop ((n 300)) (if (= n 0) 'finished (loop (- n 1))))))",
            env, &err);
        lisp_value spinner =
            lisp_eval_string("(spawn (lambda () (let loop () (loop))))", env, &err);

        int passes = lisp_sched_run(&s, 40);  // capped: the spinner never ends

        check_str("terminating sibling finished", render(lisp_ctx_value(finite), buf, sizeof buf),
                  "finished");
        check_true("sibling reached DONE", lisp_ctx_state(finite) == LISP_CTX_DONE);
        check_true("spinner was preempted, not finished",
                   lisp_ctx_state(spinner) != LISP_CTX_DONE &&
                       lisp_ctx_state(spinner) != LISP_CTX_ERROR);
        check_true("scheduler hit the pass cap", passes == 40);
    }

    printf("\n[lisp sched] %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
