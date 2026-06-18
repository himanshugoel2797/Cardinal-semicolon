// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// Host-built test for the K5d multi-core substrate: per-core scheduler state +
// the runtime lock + the grow-only (frozen) system heap. The host is single-
// threaded, so concurrency is *simulated* by swapping the installed core-id hook
// between cores; that exercises the per-core indexing and the lock-balance
// invariants without real parallelism (the kernel boot is the real SMP proof).
// It checks:
//
//   1. The active scheduler is PER-CORE: a context running on core 0 that spawns
//      a child lands the child on core 0's scheduler, not on a different core's
//      -- even when another core's scheduler was the most recently initialised.
//   2. The runtime lock is installed, actually used, balanced, and NEVER entered
//      recursively (the intern -> system-alloc path must not deadlock a plain
//      lock).
//   3. lisp_gc_set_multicore freezes the system heap (collection becomes a no-op)
//      and unfreezes cleanly, while PER-CONTEXT heaps keep collecting precisely.

#include <stdio.h>
#include <string.h>

#include "lisp.h"

static int failures = 0;
static int checks = 0;

static void check_true(const char *label, int cond) {
    checks++;
    if (!cond) {
        printf("  FAIL %-46s (expected true)\n", label);
        failures++;
    } else {
        printf("  ok   %-46s\n", label);
    }
}

static void check_eq(const char *label, long got, long want) {
    checks++;
    if (got != want) {
        printf("  FAIL %-46s got %ld want %ld\n", label, got, want);
        failures++;
    } else {
        printf("  ok   %-46s -> %ld\n", label, got);
    }
}

// --- installed concurrency hooks --------------------------------------------

static int g_core = 0;            // which "core" the lib currently sees
static int test_core_id(void) { return g_core; }

static int g_lock_depth = 0;      // current nesting (must never exceed 1)
static int g_lock_max = 0;        // high-water mark of nesting
static long g_lock_calls = 0;     // total lock acquisitions
static void test_lock(void) {
    g_lock_calls++;
    if (++g_lock_depth > g_lock_max)
        g_lock_max = g_lock_depth;
}
static void test_unlock(void) { g_lock_depth--; }

// Count the contexts on a scheduler's run queue (a plain list of ctx values).
static long queue_len(lisp_value q) {
    long n = 0;
    while (lisp_is_pair(q)) {
        n++;
        q = lisp_cdr(q);
    }
    return n;
}

int main(void) {
    uintptr_t stack_marker;
    lisp_gc_init(&stack_marker);
    lisp_set_concurrency(test_lock, test_unlock, test_core_id);

    printf("[lisp smp] per-core scheduler + runtime lock + frozen system heap\n");

    // One shared environment, built on core 0 (the kernel shares a single env
    // across cores too; contexts treat it as read-only).
    g_core = 0;
    lisp_value env = lisp_default_env();
    lisp_install_sched(env);
    const char *err = NULL;

    // 1. Per-core scheduler. Initialise core 1's scheduler LAST, so a single
    //    global "current scheduler" would point at it; then run a context on
    //    core 0 that spawns a child. With per-core state the child must land on
    //    core 0's scheduler (s0), leaving core 1's (s1) untouched.
    lisp_sched_t s1;
    g_core = 1;
    lisp_sched_init(&s1, 64);
    s1.per_context_heaps = 1;

    lisp_sched_t s0;
    g_core = 0;
    lisp_sched_init(&s0, 64);
    s0.per_context_heaps = 1;

    // A parent that spawns a child and then returns 'parent. spawn/cur-scheduler
    // are resolved from the running core (0).
    lisp_value parent = lisp_eval_string(
        "(spawn (lambda () (begin (spawn (lambda () 'child)) 'parent)))", env, &err);
    check_true("parent spawned without error", err == NULL);

    g_core = 0;
    lisp_sched_run(&s0, 0);  // runs parent, then the child it spawned, to completion

    // Both the parent and the child it spawned are on core 0's queue; core 1's
    // queue never received the child.
    check_eq("child landed on core 0's scheduler", queue_len(s0.queue), 2);
    check_eq("core 1's scheduler untouched", queue_len(s1.queue), 0);
    {
        char buf[32];
        lisp_print(lisp_ctx_value(parent), buf, sizeof buf);
        check_true("parent finished -> parent", strcmp(buf, "parent") == 0);
    }

    // 2. The lock was actually exercised (env build + spawns intern symbols and
    //    allocate into the system heap), stayed balanced, and was never nested.
    check_true("runtime lock was used", g_lock_calls > 0);
    check_eq("lock balanced (depth 0)", g_lock_depth, 0);
    check_eq("lock never recursive (max depth 1)", g_lock_max, 1);

    // 3a. Freezing the system heap makes a forced system collection a no-op
    //     (deterministic via the collection counter), and unfreezing restores it.
    g_core = 0;
    size_t c_before = lisp_gc_collections();
    lisp_gc_set_multicore(1);
    lisp_gc_collect();
    check_eq("frozen: system collection is a no-op", (long)lisp_gc_collections(), (long)c_before);

    // 3b. While the system heap is frozen, a per-context heap STILL collects
    //     precisely (this is what keeps multi-core memory bounded). Run a context
    //     that allocates well past the per-heap threshold.
    lisp_sched_t s2;
    g_core = 0;
    lisp_sched_init(&s2, 100000);
    s2.per_context_heaps = 1;
    lisp_value worker = lisp_eval_string(
        "(spawn (lambda () (let loop ((i 0) (n 200000)) (if (= n 0) i (loop (+ i 1) (- n 1))))))",
        env, &err);
    g_core = 0;
    lisp_sched_run(&s2, 0);
    check_true("frozen system heap: per-context heap still collected",
               lisp_ctx_heap_collections(worker) > 0);
    {
        char buf[32];
        lisp_print(lisp_ctx_value(worker), buf, sizeof buf);
        check_true("worker finished -> 200000", strcmp(buf, "200000") == 0);
    }

    // 3c. Unfreezing restores system collection.
    g_core = 0;
    size_t c_thaw = lisp_gc_collections();
    lisp_gc_set_multicore(0);
    lisp_gc_collect();
    check_eq("thawed: system collection runs again", (long)lisp_gc_collections(), (long)(c_thaw + 1));

    // The lock must still be balanced after all the GC traffic above.
    check_eq("lock still balanced after GC", g_lock_depth, 0);
    check_eq("lock still never recursive", g_lock_max, 1);

    printf("[lisp smp] %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
