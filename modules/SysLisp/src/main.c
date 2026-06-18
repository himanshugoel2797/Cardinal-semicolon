// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// SysLisp -- the kernel-resident Scheme runtime, folded into the OS. K4 wrapped
// the host-proven runtime (interpreter + scheduler + per-context GC) as a signed
// module that self-tested on boot. K5 (step 1) makes it a PERSISTENT runtime: a
// long-lived kernel task runs the Lisp scheduler, and a native interrupt handler
// drives Lisp contexts through the native-ISR -> event -> wake-context bridge.
//
// This is the mechanism the "interpreter IS the scheduler" end state is built on;
// it runs as a task ALONGSIDE SysTaskMgr (which still schedules the OS's native
// tasks) so the system stays fully bootable while drivers are migrated to Lisp
// one at a time. Replacing SysTaskMgr's native scheduler outright is the last
// step, once nothing native remains.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "lisp.h"

#include "SysTimer/timer.h"

// Kernel services resolved at module-load time (this module is already verified).
// SysLisp no longer depends on the native task API (task_create/yield/monitor):
// it IS the per-core scheduler loop now, entered via CALL from the boot script.
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

// --- Foreign-function examples: kernel services as Lisp primitives ------------

// (uptime-ns) -> nanoseconds since boot, as an exact integer. The fixnum range is
// 62-bit (~146 years of ns), so no truncation in practice; a no-counter timer
// returns the (uint64_t)-1 sentinel, surfaced here as an error.
static lisp_value prim_uptime_ns(lisp_value *a, int n, const char **e) {
    (void)a;
    if (n != 0)
        return (*e = "uptime-ns: expects no arguments"), LISP_UNDEF;
    uint64_t ns = timer_timestamp_ns();
    if (ns == (uint64_t)-1)  // TIMER_NO_COUNTER
        return (*e = "uptime-ns: no readable timer"), LISP_UNDEF;
    return lisp_fixnum((int64_t)ns);
}

// --- The native-ISR -> event -> wake-context bridge ---------------------------
//
// A minimal native ISR is NOT a Lisp evaluator: it runs in interrupt context
// where allocation/GC are illegal. It only POSTS an event -- here, bumps a
// counter and clears the waiting context's blocked flag (two word writes) -- and
// a Lisp context resumes in normal task context. This is the universal completion
// path the whole async-yield model rides on (the same shape serves DMA/disk/NIC
// completions, timers, and inter-context messages).

static volatile uint32_t g_event_count = 0;     // bumped by the ISR; the scheduler monitors it
static volatile lisp_value g_event_waiter = 0;  // the parked context to wake (0 = none)

static void lisp_event_isr(int irq) {
    (void)irq;
    g_event_count++;
    lisp_value w = g_event_waiter;
    if (w != 0)
        lisp_ctx_wake(w);  // ISR-safe: a single word write
}

// (%event-count) -> the current hardware-event counter.
static lisp_value prim_event_count(lisp_value *a, int n, const char **e) {
    (void)a;
    (void)n;
    (void)e;
    return lisp_fixnum((int64_t)g_event_count);
}

// (%event-wait) -> register the running context as the event waiter and park it.
// It is woken by the next lisp_event_isr. The Lisp `wait-event` wrapper below
// loops over %event-count so no event is ever missed (count-based, not edge).
static lisp_value prim_event_wait(lisp_value *a, int n, const char **e) {
    (void)a;
    (void)n;
    lisp_value self = lisp_current_ctx();
    if (self == LISP_EMPTY)
        return (*e = "%event-wait: not under the scheduler"), LISP_UNDEF;
    g_event_waiter = self;
    lisp_ctx_block(self);
    return LISP_UNDEF;
}

// --- Self-test ----------------------------------------------------------------

static int g_pass = 0;
static int g_fail = 0;

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

static void run_self_test(lisp_value env) {
    check(env, "(+ 1 2 3)", "6");
    check(env, "(define (fact n) (if (= n 0) 1 (* n (fact (- n 1))))) (fact 6)", "720");
    check(env, "(map (lambda (x) (* x x)) '(1 2 3 4))", "(1 4 9 16)");
    check(env, "(let loop ((i 0) (n 1000)) (if (= n 0) i (loop (+ i 1) (- n 1))))", "1000");
    check(env, "(+ 1.5 2.25)", "3.75");
    check(env, "(/ 7 2)", "3.5");
    check(env, "(begin (display \"hello from lisp\") (newline) 'displayed)", "displayed");
    check(env, "(define keep (list 1 2 3 4 5))", "keep");
    lisp_gc_collect();
    lisp_gc_collect();
    check(env, "(apply + keep)", "15");
    check(env, "(> (uptime-ns) 0)", "#t");
    check_scheduler(env);

    char num[24];
    print_str("[SysLisp] ");
    lisp_print(lisp_fixnum(g_pass), num, sizeof num);
    print_str(num);
    print_str(" passed, ");
    lisp_print(lisp_fixnum(g_fail), num, sizeof num);
    print_str(num);
    print_str(" failed\r\n");
    print_str(g_fail == 0 ? "[SysLisp] ALL TESTS PASSED\r\n" : "[SysLisp] SELF-TEST FAILED\r\n");
}

// --- Persistent scheduler task ------------------------------------------------

// Drive Lisp contexts until the ISR-waiting `waiter` finishes. When no context is
// runnable (the waiter is parked on a hardware event), sleep until the next event
// via task_monitor on the event counter; the timer ISR bumps it and the native
// scheduler re-runs us. This is exactly the loop a full per-core Lisp scheduler
// runs; here it hosts one event-driven context + one CPU-bound context.
static void run_isr_demo(lisp_value env) {
    // Arm a one-shot HPET timer (~10ms) as a stand-in hardware event source; its
    // ISR wakes a parked Lisp context. (Periodic timers aren't free here -- the
    // local APIC timer is the OS scheduler's -- so this is a single event, which
    // is all that is needed to prove the bridge: a native ISR resuming a context.)
    int vec = timer_request(timer_features_oneshot, 10000000ull, lisp_event_isr);
    if (vec < 0) {
        print_str("[SysLisp] (no interrupt timer available; skipping ISR demo)\r\n");
        return;
    }

    lisp_sched_t s;
    lisp_sched_init(&s, 200);
    s.per_context_heaps = 1;
    const char *err = NULL;
    lisp_eval_string(
        // Parks until the event counter advances (count-based, so the wake is
        // never lost even if the ISR fires before the context parks):
        "(define waiter"
        "  (spawn (lambda () (let ((start (%event-count)))"
        "                      (let loop () (if (> (%event-count) start) 'woke-by-isr"
        "                                       (begin (%event-wait) (loop))))))))"
        // A concurrent CPU-bound context that runs to completion WHILE the waiter
        // is parked -- cooperative concurrency under the Lisp scheduler:
        "(define worker"
        "  (spawn (lambda () (let loop ((i 0) (k 20000)) (if (= k 0) i (loop (+ i 1) (- k 1)))))))",
        env, &err);
    lisp_value waiter = lisp_eval_string("waiter", env, &err);
    lisp_value worker = lisp_eval_string("worker", env, &err);

    // Bound the wait with the (counter-based, interrupt-free) timestamp so a timer
    // ISR that never gets delivered cannot hang the boot.
    uint64_t deadline = timer_timestamp_ns() + 2000000000ull;  // 2s
    for (;;) {
        uint32_t seen = g_event_count;  // capture BEFORE running, so no wakeup is lost
        lisp_sched_run(&s, 0);          // run all runnable contexts until parked/done
        if (lisp_ctx_state(waiter) == LISP_CTX_DONE) {
            char wb[64], kb[64];
            lisp_print(lisp_ctx_value(waiter), wb, sizeof wb);
            lisp_print(lisp_ctx_value(worker), kb, sizeof kb);
            print_str("[SysLisp]  ok  context woken by a timer ISR   -> ");
            print_str(wb);
            print_str("\r\n[SysLisp]  ok  concurrent worker completed    -> ");
            print_str(kb);
            print_str("\r\n[SysLisp] PERSISTENT SCHEDULER + ISR BRIDGE OK\r\n");
            g_event_waiter = 0;  // the waiter is gone; stop the ISR touching it
            return;
        }
        // The waiter is parked on the event: enable interrupts and wait for the
        // ISR to bump the counter (busy-poll, deadline-bounded so a non-delivered
        // interrupt can't wedge us). A real driver IRQ uses the same wake path.
        __asm__ volatile("sti");
        while (g_event_count == seen && timer_timestamp_ns() < deadline)
            __asm__ volatile("pause");
        if (g_event_count == seen) {  // deadline hit without a tick
            print_str("[SysLisp] (timer ISR not delivered in time; ending ISR demo)\r\n");
            g_event_waiter = 0;
            return;
        }
    }
}

// THE PER-CORE SCHEDULER LOOP. Called once via `CALL:lisp_scheduler_enter` at the
// end of the boot script, on the boot thread itself -- it NEVER returns. This is
// the K5 flip: the interpreter is the scheduler. There is no native task switcher
// underneath; this single native thread runs Lisp contexts (which context-switch
// among themselves at safe points via the explicit-stack machine), and its frame
// is the conservative GC's stack base. FP is safe with no native preemption to
// clobber SSE state, exactly as the design intends.
int lisp_scheduler_enter() {
    lisp_gc_init(__builtin_frame_address(0));
    lisp_set_output(lisp_out, NULL);

    print_str("\r\n[SysLisp] Lisp scheduler is the per-core loop "
              "(native task switcher not started)\r\n");

    lisp_value env = lisp_default_env();
    if (env != LISP_UNDEF) {
        lisp_install_sched(env);
        lisp_env_define(env, lisp_make_symbol("uptime-ns", 9),
                        lisp_make_primitive(prim_uptime_ns, "uptime-ns"));
        lisp_env_define(env, lisp_make_symbol("%event-count", 12),
                        lisp_make_primitive(prim_event_count, "%event-count"));
        lisp_env_define(env, lisp_make_symbol("%event-wait", 11),
                        lisp_make_primitive(prim_event_wait, "%event-wait"));

        run_self_test(env);
        run_isr_demo(env);
    } else {
        print_str("[SysLisp] FAIL: could not build the default environment\r\n");
    }

    // The runtime stays resident as the per-core loop. With drivers off there are
    // no persistent Lisp contexts yet, so idle waiting for interrupts; a migrated
    // driver's IRQ will wake a context here. Never returns.
    print_str("[SysLisp] idle (no Lisp contexts pending; awaiting events)\r\n");
    for (;;)
        __asm__ volatile("sti; hlt");
    return 0;  // unreached
}

int module_init() {
    // The runtime is entered as the per-core scheduler loop by the boot script's
    // CALL:lisp_scheduler_enter (which never returns), not from here.
    print_str("[SysLisp] loaded (runtime enters via CALL:lisp_scheduler_enter)\r\n");
    return 0;
}
