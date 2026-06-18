// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// SysLisp -- the kernel-resident Scheme runtime, folded into the OS. K4 wrapped
// the host-proven runtime as a signed module that self-tested on boot; K5c made
// it the per-core scheduler loop (the boot thread CALLs lisp_scheduler_enter and
// never returns -- the interpreter IS the scheduler). K5d takes it MULTI-CORE:
// one Lisp scheduler loop per CPU.
//
// Each core runs its own scheduler over its own contexts (in their own precisely-
// collected per-context heaps). The only state shared between cores is the
// interned-symbol table and the system heap, guarded by a single runtime lock
// installed below; once the secondary cores are released the system heap is
// FROZEN (grow-only) because its conservative collector cannot see another core's
// stack roots (see notes/core/lisp-substrate.md, K5d). Cores are otherwise
// independent islands -- cross-core messaging is a later step.
//
// KNOWN FIRST-CUT LIMITATIONS (correctness is solid; these are performance, and
// are deferred -- the plan's "global lock first, revisit if contention shows"):
//   - The single runtime lock also guards the GC mark scratch, so per-context
//     collections SERIALISE across cores even though the heaps are disjoint.
//     Per-core scratch would let them run in parallel.
//   - The kernel allocator (modules/SysMemory) is O(n) best-fit with no free-list
//     coalescing (mem_compact is a TODO), so heavy GC churn from many cores
//     degrades super-linearly. A big proof loop (n>>1e4) at 4 cores can take many
//     seconds; the per-core proof below is deliberately kept modest.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <cardinal/local_spinlock.h>

#include "lisp.h"

#include "SysTimer/timer.h"
#include "SysMP/mp.h"
#include "SysInterrupts/interrupts.h"

// Kernel services resolved at module-load time (this module is already verified).
// SysLisp no longer depends on the native task API (task_create/yield/monitor):
// it IS the per-core scheduler loop now, entered via CALL from the boot script.
int print_str(const char *s);
uint64_t timer_timestamp_ns(void);

// --- Runtime lock + shared environment ----------------------------------------

// The single lock guarding the runtime's cross-core shared state (system heap +
// intern table + GC scratch). A plain spinlock is sufficient: only the per-core
// scheduler loops (task context) take it, never the event ISR -- which does just
// an ISR-safe word write via lisp_ctx_wake -- so it can't be re-entered from an
// interrupt and needs no cli().
static int g_runtime_lock = 0;
static void lisp_lock(void) { local_spinlock_lock(&g_runtime_lock); }
static void lisp_unlock(void) { local_spinlock_unlock(&g_runtime_lock); }

// The one shared global environment, built once on the BSP and read (never
// mutated) by every core's contexts. Living in the frozen system heap, it is a
// permanent root; per-context heaps mark-stop at it.
static lisp_value g_env = LISP_EMPTY;

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
// The single parked context to wake (0 = none). One slot is fine today: %event-wait
// is used only by run_isr_demo in the single-core phase. TODO(multi-core): when a
// migrated driver parks contexts from multiple cores, replace this with a per-core
// (or per-IRQ) waiter table so two waiters can't race the one slot.
static volatile lisp_value g_event_waiter = 0;

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

// --- ps2 keyboard driver, migrated to a Lisp context --------------------------
//
// The ps2 module (loaded before us) does the controller init + decode + queueing
// in its IRQ; here we bridge it to the Lisp input service. The keyboard IRQ fires
// ps2_wake_hook (ISR context: one word write) to wake the parked ps2 driver
// CONTEXT, which then drains events with (%ps2-poll) and forwards them as messages
// to the coreinput context. This is the ISR -> wake -> poll -> send path -- the
// first real-hardware exercise of the wake bridge.
void ps2_set_irq_hook(void (*hook)(void));  // exported by the ps2 module
int ps2_poll_key(int *code, int *pressed);
int ps2_pending(void);

static volatile lisp_value g_ps2_waiter = 0;  // the parked ps2 context (0 = none)

static void ps2_wake_hook(void) {  // ISR context
    lisp_value w = g_ps2_waiter;
    if (w != 0)
        lisp_ctx_wake(w);
}

// (%ps2-poll) -> (key <scancode> <pressed?>) for the next event, or #f if none.
static lisp_value prim_ps2_poll(lisp_value *a, int n, const char **e) {
    (void)a;
    (void)n;
    (void)e;
    int code = 0, pressed = 0;
    if (!ps2_poll_key(&code, &pressed))
        return LISP_FALSE;
    // Built in the running (ps2) context's heap; deep-copied into coreinput's on
    // send. (key code pressed)
    lisp_value tail = lisp_cons(lisp_fixnum(code), lisp_cons(lisp_fixnum(pressed), LISP_EMPTY));
    return lisp_cons(lisp_make_symbol("key", 3), tail);
}

// (%ps2-wait) -> park the running context until the next keyboard IRQ, UNLESS an
// event is already pending (so a key landing just before we park is not missed).
// cli() closes the check-then-park window against the same-core IRQ; the ps2 IRQ
// is delivered to the BSP, where this context runs. (A keyboard IRQ routed to a
// different core than the waiter could still miss -- acceptable until cross-core
// messaging lands; the input service is BSP-pinned for now.)
static lisp_value prim_ps2_wait(lisp_value *a, int n, const char **e) {
    (void)a;
    (void)n;
    lisp_value self = lisp_current_ctx();
    if (self == LISP_EMPTY)
        return (*e = "%ps2-wait: not under the scheduler"), LISP_UNDEF;
    g_ps2_waiter = self;
    int cli_state = cli();
    if (ps2_pending()) {  // an event arrived; stay runnable and re-drain
        sti(cli_state);
        return LISP_FALSE;
    }
    lisp_ctx_block(self);  // blocked=1, budget=0 -> suspends at the next safe point
    sti(cli_state);
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

// --- The per-core scheduler loop ----------------------------------------------

// Build "[SysLisp] core <id> online: ... proof -> <result>" in one buffer and
// emit it with a single print_str, so concurrent cores' lines never interleave
// (one print_str call is atomic on COM1) and we avoid nesting the serial lock
// inside the runtime lock. `append` truncates safely on overflow.
static void announce_core(int id, const char *err, lisp_value proof) {
    char line[160];
    char num[24];
    size_t p = 0;
#define APP(s)                                              \
    do {                                                    \
        const char *x_ = (s);                               \
        while (*x_ != '\0' && p + 1 < sizeof line)          \
            line[p++] = *x_++;                              \
    } while (0)
    APP("[SysLisp] core ");
    lisp_print(lisp_fixnum(id), num, sizeof num);
    APP(num);
    APP(" online: lisp scheduler running, proof -> ");
    if (err == NULL && proof != LISP_UNDEF) {
        lisp_print(lisp_ctx_value(proof), num, sizeof num);
        APP(num);
    } else {
        APP("ERROR");
    }
    APP("\r\n");
#undef APP
    line[p] = '\0';
    print_str(line);
}

// --- Servers-as-Lisp: the async input service (first server migration) --------
//
// CoreInput becomes a long-lived Lisp CONTEXT instead of a native task with a
// synchronous callback ABI. Drivers (also contexts) are shared-nothing, so the
// old re-entrant callbacks become MESSAGES: a driver `send`s
//   (register <name>)        -- announce itself, and
//   (event <payload>)        -- one input event,
// to the coreinput context, which keeps a device table and routes events. There
// is no synchronous re-entry, so the network-style "rx handler calls back into tx"
// self-deadlock cannot occur here by construction.
//
// The whole thing is set up as ONE expression evaluated in the shared env: it only
// LOOKS UP names there (spawn/recv/cond/...), never `define`s into it -- mutating
// the cross-core-shared env after the APs are live would be a data race. The
// coordinator handle is held in a `let` binding the ps2 driver context closes
// over, not in a global. The ps2 context registers, then pumps: it drains
// (%ps2-poll) and forwards each event, parking on (%ps2-wait) -- woken by the
// keyboard IRQ via ps2_wake_hook -- when the queue is empty.
static void setup_input_service(void) {
    const char *err = NULL;
    lisp_eval_string(
        "(let ((coreinput"
        "        (spawn (lambda ()"
        "          (let loop ((devs '()))"
        "            (let ((m (recv)))"
        "              (cond ((eq? (car m) 'register)"
        "                     (display \"[coreinput] device registered: \")"
        "                     (display (cadr m)) (newline)"
        "                     (loop (cons (cadr m) devs)))"
        "                    ((eq? (car m) 'event)"
        "                     (display \"[coreinput] event \") (display (cadr m)) (newline)"
        "                     (loop devs))"
        "                    (else (loop devs))))))))) "
        // Slice-1b: the migrated ps2 driver context -- register, then pump: drain
        // (%ps2-poll), forward each event to coreinput, park on (%ps2-wait) (woken
        // by the keyboard IRQ) when the queue is empty.
        "  (spawn (lambda ()"
        "    (send coreinput (list 'register 'ps2-keyboard))"
        "    (let pump ()"
        "      (let ((e (%ps2-poll)))"
        "        (if e"
        "            (begin (send coreinput (list 'event e)) (pump))"
        "            (begin (%ps2-wait) (pump))))))))",
        g_env, &err);
    if (err != NULL) {
        print_str("[SysLisp] input service setup error: ");
        print_str(err);
        print_str("\r\n");
    }
}

// THE PER-CORE SCHEDULER LOOP, run by EVERY core (the BSP at the tail of
// lisp_scheduler_enter, each AP once released). It NEVER returns. This core
// runs its OWN scheduler over its OWN contexts (each in its own precisely-
// collected heap); the shared system heap + intern table are guarded by the
// runtime lock. There is no native task switcher underneath -- one native thread
// per core runs Lisp contexts that context-switch among themselves at safe
// points. FP is safe with no native preemption to clobber SSE state.
static void NORETURN lisp_core_loop(void) {
    int id = interrupt_get_cpu_idx();

    // This core's scheduler. A stack local (its run queue is a GC root reachable
    // from this frame); per_context_heaps gives each spawned context its own heap.
    lisp_sched_t sched;
    lisp_sched_init(&sched, 256);
    sched.per_context_heaps = 1;

    // Long-lived OS services live on the BSP for now (cross-core messaging is a
    // later step, so a service + its drivers must share one core). Spawn them onto
    // this scheduler before the proof so they run in the same pass.
    if (id == 0)
        setup_input_service();

    // Per-core proof of life: spawn a context that does a real (heap-allocating,
    // per-context-GC-exercising) computation in the SHARED environment, run it to
    // completion on THIS core's scheduler, and report the result with the core id.
    // Kept modest: it's a liveness check, not a benchmark -- per-context GC across
    // all cores currently serialises on the one runtime lock (see the file header).
    const char *err = NULL;
    lisp_value proof = lisp_eval_string(
        "(spawn (lambda () (let loop ((i 0) (n 2000)) (if (= n 0) i (loop (+ i 1) (- n 1))))))",
        g_env, &err);
    lisp_sched_run(&sched, 0);

    // Announce on a single line, built in a local buffer and emitted with ONE
    // print_str. print_str itself serialises a whole call on COM1, so distinct
    // cores' announcements don't interleave -- and crucially we do NOT hold the
    // runtime lock across print_str (which takes its own serial lock under cli());
    // nesting those two locks would impose a fragile g_runtime_lock -> serial-lock
    // ordering.
    announce_core(id, err, proof);

    // Resident scheduler loop: run any runnable contexts, then idle until an
    // interrupt (a migrated driver's IRQ wakes a parked context via the ISR
    // bridge). With drivers off there are no persistent contexts yet, so this
    // simply idles. Never returns.
    for (;;) {
        lisp_sched_run(&sched, 64);
        __asm__ volatile("sti; hlt");
    }
}

// Each application processor, parked in mp_signalready() since boot, is released
// straight into lisp_core_loop and joins as another Lisp scheduler core. It needs
// no extra per-core hardware bring-up: apscript already ran intr_mp_init (which
// allocates this core's GDT/TSS and the exception IST stacks via gdt_init) and
// fp_mp_init, and a ring-0-only Lisp core never uses TSS rsp0 (that is only the
// ring3->ring0 stack). No native run queue / idle task / preemption timer either
// -- that machinery is dormant under the interpreter-as-scheduler model. The
// system heap is already frozen and per-context heaps are precise, so an AP also
// needs no GC stack base.

// THE ENTRY POINT, called once via `CALL:lisp_scheduler_enter` at the end of the
// boot script, on the BSP boot thread -- it NEVER returns. It performs the one-
// time global runtime init (single-core, so the system collector is live for the
// self-test), then goes multi-core: freeze the system heap, release the APs into
// lisp_core_loop, and run this (BSP) core's own scheduler loop.
int lisp_scheduler_enter() {
    // Install the concurrency hooks FIRST, so all subsequent interning/allocation
    // is already lock-guarded (uncontended while single-core) and every core
    // resolves to its own per-core scheduler slot by APIC id.
    lisp_set_concurrency(lisp_lock, lisp_unlock, interrupt_get_cpu_idx);
    lisp_gc_init(__builtin_frame_address(0));
    lisp_set_output(lisp_out, NULL);

    print_str("\r\n[SysLisp] interpreter-as-scheduler, multi-core bring-up\r\n");

    g_env = lisp_default_env();
    if (g_env == LISP_UNDEF) {
        print_str("[SysLisp] FAIL: could not build the default environment\r\n");
        for (;;)
            __asm__ volatile("cli; hlt");
    }
    lisp_install_sched(g_env);
    lisp_env_define(g_env, lisp_make_symbol("uptime-ns", 9),
                    lisp_make_primitive(prim_uptime_ns, "uptime-ns"));
    lisp_env_define(g_env, lisp_make_symbol("%event-count", 12),
                    lisp_make_primitive(prim_event_count, "%event-count"));
    lisp_env_define(g_env, lisp_make_symbol("%event-wait", 11),
                    lisp_make_primitive(prim_event_wait, "%event-wait"));
    lisp_env_define(g_env, lisp_make_symbol("%ps2-poll", 9),
                    lisp_make_primitive(prim_ps2_poll, "%ps2-poll"));
    lisp_env_define(g_env, lisp_make_symbol("%ps2-wait", 9),
                    lisp_make_primitive(prim_ps2_wait, "%ps2-wait"));
    // Route the ps2 keyboard IRQ to wake the (soon-to-be-spawned) ps2 context.
    ps2_set_irq_hook(ps2_wake_hook);

    // Single-core phase: self-test + the ISR-bridge demo run with the system
    // collector still active (the BSP is the only core building shared state).
    run_self_test(g_env);
    run_isr_demo(g_env);

    // Go multi-core: freeze the (now fully-built) shared system heap -- its
    // conservative collector can't see other cores' stacks -- then release the
    // APs. They were parked in mp_signalready() since boot; each now runs
    // lisp_core_loop and becomes another Lisp scheduler core. (lisp_core_loop is
    // NORETURN; the cast to the plain ap-entry pointer type drops only that
    // attribute.)
    lisp_gc_set_multicore(1);
    print_str("[SysLisp] system heap frozen; releasing APs as Lisp cores\r\n");
    mp_set_ap_entry((void (*)(void))lisp_core_loop);

    // The BSP becomes its own Lisp scheduler core. Never returns.
    lisp_core_loop();
    return 0;  // unreached
}

int module_init() {
    // The runtime is entered as the per-core scheduler loop by the boot script's
    // CALL:lisp_scheduler_enter (which never returns), not from here.
    print_str("[SysLisp] loaded (runtime enters via CALL:lisp_scheduler_enter)\r\n");
    return 0;
}
