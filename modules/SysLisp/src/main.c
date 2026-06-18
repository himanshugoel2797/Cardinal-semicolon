// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// SysLisp -- the kernel-resident Scheme runtime, folded into the OS (K4 of the
// process-model bring-up; see notes/core/lisp-substrate.md). The interpreter,
// scheduler, and per-context GC were proven host-first (libs/lisp); this module
// wraps that static library as a signed Sys* module, points its output sink at
// the COM1 debug log, and runs an in-OS self-test of the whole stack at load time.
//
// It runs during the boot load script, in task context with the FPU initialized
// (so the runtime's hardware-double flonums are legal) and the kernel heap up (so
// the GC's malloc works), but before the scheduler preempts -- the self-test runs
// to completion synchronously.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "lisp.h"

// Kernel services resolved at module-load time (the security boundary already
// verified this module). print_str is the COM1 debug sink; timer_timestamp_ns is
// exposed to Lisp below as a small foreign-function (host-function) example.
int print_str(const char *s);
uint64_t timer_timestamp_ns(void);

// --- Output sink: route (display)/(write)/(newline) to the debug log ----------

// lisp_output_fn hands us a byte run that may not be NUL-terminated and may
// exceed any fixed buffer, so copy in chunks and NUL-terminate for print_str.
// (print_str is a C string, so a Lisp string containing an embedded NUL would be
// truncated at it; acceptable for a debug sink -- such strings are not expected.)
static void lisp_out(const char *s, size_t len, void *ctx) {
    (void)ctx;
    char buf[129];
    while (len > 0) {
        size_t n = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
        memcpy(buf, s, n);
        buf[n] = '\0';
        print_str(buf);
        s += n;
        len -= n;
    }
}

// --- Foreign-function example: a kernel service as a Lisp primitive -----------

// (uptime-ns) -> nanoseconds since boot, as an exact integer. Demonstrates the
// "expose existing C services as host functions" path. The fixnum range is 62-bit
// (~146 years of ns), so no truncation in practice; a no-counter timer returns the
// (uint64_t)-1 sentinel, surfaced here as an error rather than a bogus -1.
static lisp_value prim_uptime_ns(lisp_value *a, int n, const char **e) {
    (void)a;
    if (n != 0)
        return (*e = "uptime-ns: expects no arguments"), LISP_UNDEF;
    uint64_t ns = timer_timestamp_ns();
    if (ns == (uint64_t)-1)  // TIMER_NO_COUNTER
        return (*e = "uptime-ns: no readable timer"), LISP_UNDEF;
    return lisp_fixnum((int64_t)ns);
}

// --- Self-test ---------------------------------------------------------------

static int g_pass = 0;
static int g_fail = 0;

// Evaluate `src` and compare its printed result to `want`.
static void check(lisp_value env, const char *src, const char *want) {
    const char *err = NULL;
    lisp_value r = lisp_eval_string(src, env, &err);
    char buf[160];
    if (err != NULL) {
        print_str("[SysLisp] FAIL ");
        print_str(src);
        print_str("  -> error: ");
        print_str(err);
        print_str("\r\n");
        g_fail++;
        return;
    }
    lisp_print(r, buf, sizeof buf);
    if (strcmp(buf, want) == 0) {
        print_str("[SysLisp]  ok  ");
        print_str(src);
        print_str("  -> ");
        print_str(buf);
        print_str("\r\n");
        g_pass++;
    } else {
        print_str("[SysLisp] FAIL ");
        print_str(src);
        print_str("  -> ");
        print_str(buf);
        print_str(" (want ");
        print_str(want);
        print_str(")\r\n");
        g_fail++;
    }
}

// Exercise the cooperative scheduler + copy-on-send IPC + per-context GC in the
// kernel: a consumer blocks in (recv) and a producer sends it 1..5; the consumer
// drains and sums them. Each context gets its own heap.
static void check_scheduler(lisp_value env) {
    lisp_sched_t s;
    lisp_sched_init(&s, 100);
    s.per_context_heaps = 1;
    const char *err = NULL;
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
    char buf[64];
    lisp_print(lisp_ctx_value(consumer), buf, sizeof buf);
    if (err == NULL && strcmp(buf, "15") == 0) {
        print_str("[SysLisp]  ok  scheduler producer/consumer  -> ");
        print_str(buf);
        print_str("\r\n");
        g_pass++;
    } else {
        print_str("[SysLisp] FAIL scheduler producer/consumer  -> ");
        print_str(buf);
        print_str("\r\n");
        g_fail++;
    }
}

int module_init() {
    // The conservative system-heap collector scans the C stack from here up; this
    // is the outermost frame that holds the runtime's roots (the global env).
    lisp_gc_init(__builtin_frame_address(0));
    lisp_set_output(lisp_out, NULL);

    print_str("\r\n[SysLisp] kernel-resident Scheme: in-OS self-test\r\n");

    lisp_value env = lisp_default_env();
    if (env == LISP_UNDEF) {
        print_str("[SysLisp] FAIL: could not build the default environment\r\n");
        return -1;
    }
    lisp_install_sched(env);
    lisp_env_define(env, lisp_make_symbol("uptime-ns", 9),
                    lisp_make_primitive(prim_uptime_ns, "uptime-ns"));

    // Core evaluation: arithmetic, recursion, higher-order, tail loops.
    check(env, "(+ 1 2 3)", "6");
    check(env, "(define (fact n) (if (= n 0) 1 (* n (fact (- n 1))))) (fact 6)", "720");
    check(env, "(map (lambda (x) (* x x)) '(1 2 3 4))", "(1 4 9 16)");
    check(env, "(let loop ((i 0) (n 1000)) (if (= n 0) i (loop (+ i 1) (- n 1))))", "1000");

    // Flonums: proves the runtime's hardware doubles (SSE) work in task context.
    check(env, "(+ 1.5 2.25)", "3.75");
    check(env, "(/ 7 2)", "3.5");

    // Output sink: (display)/(write) reach the debug log.
    check(env, "(begin (display \"hello from lisp\") (newline) 'displayed)", "displayed");

    // Garbage collection: a named structure survives forced collections.
    check(env, "(define keep (list 1 2 3 4 5))", "keep");
    lisp_gc_collect();
    lisp_gc_collect();
    check(env, "(apply + keep)", "15");

    // Foreign function: a real kernel service called from Lisp.
    check(env, "(> (uptime-ns) 0)", "#t");

    // The scheduler / IPC / per-context GC stack.
    check_scheduler(env);

    // Summary. The "ALL TESTS PASSED" marker mirrors SysTest for log scanning.
    char num[24];
    print_str("[SysLisp] ");
    lisp_print(lisp_fixnum(g_pass), num, sizeof num);
    print_str(num);
    print_str(" passed, ");
    lisp_print(lisp_fixnum(g_fail), num, sizeof num);
    print_str(num);
    print_str(" failed\r\n");
    if (g_fail == 0)
        print_str("[SysLisp] ALL TESTS PASSED\r\n");
    else
        print_str("[SysLisp] SELF-TEST FAILED\r\n");

    return 0;
}
