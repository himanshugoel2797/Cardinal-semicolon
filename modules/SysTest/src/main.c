// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT
//
// SysTest -- in-OS unit test framework.
//
// Loaded very early (right after SysDebug) so it can host tests for as much of
// the kernel as possible: every module/server/driver loaded afterwards can call
// test_register() from its module_init. To stay loadable that early, SysTest
// hard-depends on nothing but kernel/common symbols (print/malloc/string and the
// kernel's GetBootInfo) -- the scheduler/MP/timer entry points it needs to *run*
// tests are resolved dynamically (elf_resolvefunction) at test time, by which
// point SysTaskMgr et al. are loaded. test_run_all() is wired into
// servicescript.txt and is a no-op unless the kernel was booted with the
// "cardinal.test" cmdline flag.

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <types.h>

#include "boot_information.h"
#include "elf.h"
#include "priv_test.h"

// ---- registration list -----------------------------------------------------

typedef struct test_node {
    test_entry_t entry;
    struct test_node *next;
} test_node_t;

static test_node_t *g_tests_head = NULL;
static test_node_t *g_tests_tail = NULL;
static int g_test_count = 0;
static bool g_test_mode = false;

// ---- dynamically-resolved scheduler/MP ops ---------------------------------
//
// Layout mirror of SysTaskMgr's semaphore_t (modules/inc/SysTaskMgr/task.h). We
// only ever pass pointers to the resolved semaphore_* ops, so the layout must
// match but we avoid importing the symbols directly.
typedef struct {
    volatile uint32_t count;
    int spinlock;
} systest_sem_t;
// Tripwire: if SysTaskMgr's semaphore_t grows/shrinks, the resolved semaphore_*
// ops would read/write past this mirror. Keep the layouts in lockstep.
_Static_assert(sizeof(systest_sem_t) == 8, "systest_sem_t must match SysTaskMgr semaphore_t");

typedef uint64_t systest_id_t;
#define SYSTEST_PERM_KERNEL 1 // task_permissions_kernel

typedef struct {
    cs_error (*task_create)(const char *, int, systest_id_t *);
    cs_error (*task_create_oncore)(const char *, int, int, systest_id_t *);
    cs_error (*task_start)(systest_id_t, void *, void *);
    cs_error (*task_end)(systest_id_t); // optional: reap an orphaned (created-but-unstarted) task
    void (*sem_init)(systest_sem_t *);
    void (*sem_signal)(systest_sem_t *);
    void (*sem_wait)(systest_sem_t *);
    void (*task_yield)(void);
    int (*task_corecount)(void);
    int (*mp_corecount)(void);
    bool resolved_tasks;  // task create/start/sem all present
    bool resolved_percpu; // additionally task_create_oncore + corecounts present
} task_ops_t;

static task_ops_t g_ops;

static void resolve_ops(void) {
    memset(&g_ops, 0, sizeof(g_ops));
    g_ops.task_create =
        (cs_error(*)(const char *, int, systest_id_t *))elf_resolvefunction("task_create_kernel");
    g_ops.task_create_oncore =
        (cs_error(*)(const char *, int, int, systest_id_t *))elf_resolvefunction("task_create_kernel_oncore");
    g_ops.task_start =
        (cs_error(*)(systest_id_t, void *, void *))elf_resolvefunction("task_start_kernel");
    g_ops.task_end = (cs_error(*)(systest_id_t))elf_resolvefunction("task_end_kernel");
    g_ops.sem_init = (void (*)(systest_sem_t *))elf_resolvefunction("semaphore_init");
    g_ops.sem_signal = (void (*)(systest_sem_t *))elf_resolvefunction("semaphore_signal");
    g_ops.sem_wait = (void (*)(systest_sem_t *))elf_resolvefunction("semaphore_wait");
    g_ops.task_yield = (void (*)(void))elf_resolvefunction("task_yield");
    g_ops.task_corecount = (int (*)(void))elf_resolvefunction("task_corecount");
    g_ops.mp_corecount = (int (*)(void))elf_resolvefunction("mp_corecount");

    g_ops.resolved_tasks = g_ops.task_create && g_ops.task_start && g_ops.sem_init &&
                           g_ops.sem_signal && g_ops.sem_wait;
    g_ops.resolved_percpu = g_ops.resolved_tasks && g_ops.task_create_oncore &&
                            g_ops.task_corecount && g_ops.mp_corecount && g_ops.task_yield;
}

// ---- small output helpers ---------------------------------------------------

// Unsigned decimal print. (Not ltoa(): that takes a signed long long, so a value
// with bit 63 set would render as negative.)
static void print_uint(uint64_t v) {
    char buf[24];
    int i = (int)sizeof(buf);
    buf[--i] = '\0';
    if (v == 0)
        buf[--i] = '0';
    while (v > 0 && i > 0) {
        buf[--i] = (char)('0' + (v % 10));
        v /= 10;
    }
    DEBUG_PRINT(&buf[i]);
}

// ---- public assertion plumbing ---------------------------------------------

void test_report_check(test_ctx_t *ctx) {
    if (ctx != NULL)
        ctx->checks++;
}

void test_report_fail(test_ctx_t *ctx, const char *file, int line, const char *expr) {
    if (ctx != NULL)
        ctx->fails++;
    DEBUG_PRINT("    # FAIL ");
    DEBUG_PRINT(file != NULL ? file : "?");
    DEBUG_PRINT(":");
    print_uint((uint64_t)line);
    DEBUG_PRINT(": ");
    DEBUG_PRINT(expr != NULL ? expr : "");
    DEBUG_PRINT("\r\n");
}

int test_core(const test_ctx_t *ctx) {
    return ctx != NULL ? ctx->core : 0;
}

// ---- registration -----------------------------------------------------------

cs_error test_register(const test_def_t *def) {
    if (def == NULL || def->fn == NULL || def->suite == NULL || def->name == NULL)
        return CS_INVALIDARG;

    test_node_t *node = (test_node_t *)malloc(sizeof(test_node_t));
    if (node == NULL)
        return CS_OUTOFMEM;

    memset(node, 0, sizeof(*node));
    node->entry.def = *def;
    node->entry.done = false;
    node->next = NULL;

    if (g_tests_tail == NULL)
        g_tests_head = node;
    else
        g_tests_tail->next = node;
    g_tests_tail = node;
    g_test_count++;
    return CS_OK;
}

bool test_mode_active(void) {
    return g_test_mode;
}

// ---- execution --------------------------------------------------------------

typedef struct {
    test_fn_t fn;
    test_ctx_t *ctx;
    systest_sem_t *done;
} task_arg_t;

// Entry point for a TEST_RUN_TASK / TEST_RUN_PERCPU test task. Runs the test,
// signals completion, then returns -- kernel_entry_handler ends the task.
static void test_task_trampoline(void *a) {
    task_arg_t *ta = (task_arg_t *)a;
    ta->fn(ta->ctx);
    g_ops.sem_signal(ta->done);
}

// Report one finished test as a TAP line. Returns the test's failure count.
static uint32_t report_entry(int idx, test_entry_t *e) {
    bool ok = (e->fails == 0);
    DEBUG_PRINT(ok ? "ok " : "not ok ");
    print_uint((uint64_t)idx);
    DEBUG_PRINT(" - ");
    DEBUG_PRINT(e->def.suite);
    DEBUG_PRINT(".");
    DEBUG_PRINT(e->def.name);
    if (e->def.flags & TEST_FLAG_SKIP) {
        DEBUG_PRINT(" # SKIP");
    } else if (!ok) {
        DEBUG_PRINT(" (");
        print_uint(e->fails);
        DEBUG_PRINT("/");
        print_uint(e->checks);
        DEBUG_PRINT(" checks failed)");
    }
    DEBUG_PRINT("\r\n");
    return e->fails;
}

static void run_inline(test_entry_t *e) {
    struct test_ctx ctx = {.suite = e->def.suite, .name = e->def.name, .core = 0};
    e->def.fn(&ctx);
    e->checks = ctx.checks;
    e->fails = ctx.fails;
}

static void run_task(test_entry_t *e) {
    struct test_ctx ctx = {.suite = e->def.suite, .name = e->def.name, .core = 0};
    systest_sem_t sem;
    g_ops.sem_init(&sem);
    task_arg_t arg = {.fn = e->def.fn, .ctx = &ctx, .done = &sem};

    systest_id_t id = 0;
    if (g_ops.task_create("systest_task", SYSTEST_PERM_KERNEL, &id) != CS_OK) {
        ctx.checks++;
        ctx.fails++;
        DEBUG_PRINT("    # FAIL: could not create test task\r\n");
    } else if (g_ops.task_start(id, (void *)test_task_trampoline, &arg) != CS_OK) {
        // Created but not started: reap the orphan so it isn't stranded in a
        // run queue forever (it never ran, so it will never signal the sem).
        if (g_ops.task_end)
            g_ops.task_end(id);
        ctx.checks++;
        ctx.fails++;
        DEBUG_PRINT("    # FAIL: could not start test task\r\n");
    } else {
        // sem_wait blocks (deschedules) until the task signals. The arg/ctx/sem
        // live on this frame and stay valid because we do not return until then;
        // a genuinely wedged task is backstopped by the outer harness timeout.
        g_ops.sem_wait(&sem);
    }
    e->checks = ctx.checks;
    e->fails = ctx.fails;
}

// Wait (yielding) until every core SysMP knows about has joined the scheduler,
// so a per-CPU test can pin a task onto each one. Bounded; warns if short.
static int wait_for_cores(void) {
    int target = g_ops.mp_corecount();
    if (target < 1)
        target = 1;
    for (int spins = 0; spins < 2000000 && g_ops.task_corecount() < target; spins++)
        g_ops.task_yield();

    int online = g_ops.task_corecount();
    if (online < target) {
        DEBUG_PRINT("    # WARN: only ");
        print_uint((uint64_t)online);
        DEBUG_PRINT(" of ");
        print_uint((uint64_t)target);
        DEBUG_PRINT(" cores online; per-CPU test runs on the available cores\r\n");
    }
    return online;
}

static void run_percpu(test_entry_t *e) {
    int n = wait_for_cores();
    if (n < 1)
        n = 1;

    struct test_ctx *ctxs = (struct test_ctx *)malloc(sizeof(struct test_ctx) * n);
    task_arg_t *args = (task_arg_t *)malloc(sizeof(task_arg_t) * n);
    systest_sem_t sem;
    g_ops.sem_init(&sem);

    if (ctxs == NULL || args == NULL) {
        e->checks = 1;
        e->fails = 1;
        DEBUG_PRINT("    # FAIL: out of memory spawning per-CPU test\r\n");
        free(ctxs);
        free(args);
        return;
    }

    int launched = 0;
    for (int c = 0; c < n; c++) {
        ctxs[c].suite = e->def.suite;
        ctxs[c].name = e->def.name;
        ctxs[c].core = c;
        ctxs[c].checks = 0;
        ctxs[c].fails = 0;
        args[c].fn = e->def.fn;
        args[c].ctx = &ctxs[c];
        args[c].done = &sem;

        systest_id_t id = 0;
        if (g_ops.task_create_oncore("systest_percpu", SYSTEST_PERM_KERNEL, c, &id) != CS_OK) {
            ctxs[c].checks++;
            ctxs[c].fails++;
            DEBUG_PRINT("    # FAIL: could not create per-CPU task on core ");
            print_uint((uint64_t)c);
            DEBUG_PRINT("\r\n");
            continue;
        }
        if (g_ops.task_start(id, (void *)test_task_trampoline, &args[c]) != CS_OK) {
            if (g_ops.task_end) // reap the created-but-unstarted orphan
                g_ops.task_end(id);
            ctxs[c].checks++;
            ctxs[c].fails++;
            DEBUG_PRINT("    # FAIL: could not start per-CPU task on core ");
            print_uint((uint64_t)c);
            DEBUG_PRINT("\r\n");
            continue;
        }
        launched++;
    }

    for (int i = 0; i < launched; i++)
        g_ops.sem_wait(&sem);

    uint32_t checks = 0, fails = 0;
    for (int c = 0; c < n; c++) {
        checks += ctxs[c].checks;
        fails += ctxs[c].fails;
    }
    e->checks = checks;
    e->fails = fails;

    free(ctxs);
    free(args);
}

// Execute one entry according to its trigger; fills e->checks / e->fails.
static void execute_entry(test_entry_t *e) {
    if (e->def.flags & TEST_FLAG_SKIP) {
        e->checks = 0;
        e->fails = 0;
        return;
    }

    switch (e->def.run) {
    case TEST_RUN_TASK:
        if (g_ops.resolved_tasks)
            run_task(e);
        else
            run_inline(e); // degrade gracefully: run in this thread
        break;
    case TEST_RUN_PERCPU:
        if (g_ops.resolved_percpu)
            run_percpu(e);
        else
            run_inline(e);
        break;
    case TEST_RUN_INLINE:
    default:
        run_inline(e);
        break;
    }
}

int test_run_suite(const char *suite) {
    if (!g_test_mode || suite == NULL)
        return 0;

    resolve_ops();
    uint32_t fails = 0;
    int idx = 0;
    for (test_node_t *n = g_tests_head; n != NULL; n = n->next) {
        if (n->entry.done || strcmp(n->entry.def.suite, suite) != 0)
            continue;
        if (n->entry.def.flags & TEST_FLAG_DEATH)
            continue; // death tests only run from the harness-driven sweep
        execute_entry(&n->entry);
        n->entry.done = true;
        fails += report_entry(++idx, &n->entry);
    }
    return (int)fails;
}

// ---- GDB-over-CSMUX break-in pump -------------------------------------------
// In harness mode the GDB stub is tunneled over CSMUX ch2; there is no UART RX
// IRQ to drive async break-in, so a low-priority task polls gdb_poll_breakin so a
// debugger can attach and halt a running guest. Resolved at runtime (SysGdb).
static int (*g_gdb_poll)(void) = NULL;

static void gdb_pump_task(void *a) {
    (void)a;
    for (;;) {
        if (g_gdb_poll != NULL)
            g_gdb_poll();
        if (g_ops.task_yield != NULL)
            g_ops.task_yield();
    }
}

// ---- death-test phase helpers ----------------------------------------------

static int count_death_tests(void) {
    int k = 0;
    for (test_node_t *n = g_tests_head; n != NULL; n = n->next)
        if (n->entry.def.flags & TEST_FLAG_DEATH)
            k++;
    return k;
}

// The death test at 0-based index `idx` among death tests, or NULL.
static test_entry_t *death_test_at(int idx) {
    int k = 0;
    for (test_node_t *n = g_tests_head; n != NULL; n = n->next) {
        if (!(n->entry.def.flags & TEST_FLAG_DEATH))
            continue;
        if (k == idx)
            return &n->entry;
        k++;
    }
    return NULL;
}

// Run + report every NON-death test as a TAP stream. Returns failed-test count.
static int run_normal_phase(bool harness, int death_count) {
    int normal_count = g_test_count - death_count;

    DEBUG_PRINT("[SysTest] ============ test run start ============\r\n");
    DEBUG_PRINT("1..");
    print_uint((uint64_t)(harness ? normal_count : g_test_count));
    DEBUG_PRINT("\r\n");
    if (!g_ops.resolved_tasks)
        DEBUG_PRINT("[SysTest] WARN: scheduler ops unresolved; task/per-CPU tests run inline\r\n");

    uint32_t total_fail_checks = 0;
    int failed_tests = 0;
    int idx = 0;
    for (test_node_t *n = g_tests_head; n != NULL; n = n->next) {
        test_entry_t *e = &n->entry;
        if (e->def.flags & TEST_FLAG_DEATH)
            continue; // handled in the death phase
        if (e->done) {
            idx++;
            if (e->fails)
                failed_tests++;
            total_fail_checks += e->fails;
            continue;
        }
        execute_entry(e);
        e->done = true;
        uint32_t f = report_entry(++idx, e);
        if (f)
            failed_tests++;
        total_fail_checks += f;
    }

    // Without a harness, death tests cannot run -- list them as skipped so the
    // plan is complete and a reader sees they exist.
    if (!harness) {
        for (test_node_t *n = g_tests_head; n != NULL; n = n->next) {
            test_entry_t *e = &n->entry;
            if (!(e->def.flags & TEST_FLAG_DEATH))
                continue;
            DEBUG_PRINT("ok ");
            print_uint((uint64_t)++idx);
            DEBUG_PRINT(" - ");
            DEBUG_PRINT(e->def.suite);
            DEBUG_PRINT(".");
            DEBUG_PRINT(e->def.name);
            DEBUG_PRINT(" # SKIP (harness only)\r\n");
        }
    }

    DEBUG_PRINT("[SysTest] ============ ");
    print_uint((uint64_t)idx);
    DEBUG_PRINT(" tests, ");
    print_uint((uint64_t)failed_tests);
    DEBUG_PRINT(" failed, ");
    print_uint((uint64_t)total_fail_checks);
    DEBUG_PRINT(" failed checks ============\r\n");
    return failed_tests;
}

int test_run_all(void) {
    if (!g_test_mode)
        return 0; // normal boot: nothing to do, no cost

    resolve_ops();

    int death_count = count_death_tests();
    bool harness = false;
    int cursor = 0;
    if (systest_death_harness_mode())
        harness = systest_death_handshake(death_count, &cursor); // activates CSMUX, gets cursor

    // In harness mode COM1 carries framed CSMUX and COM2 is unwired, so route the
    // GDB stub over CSMUX channel 2 (if SysGdb is loaded) -- keeps the debugger
    // usable over the single multiplexed link across reboots. Also start a
    // break-in pump task so a debugger can attach asynchronously (the COM2 RX IRQ
    // that normally drives break-in is not in play over the muxed link).
    if (harness) {
        void (*gdb_use_csmux)(void) = (void (*)(void))elf_resolvefunction("gdb_use_csmux");
        if (gdb_use_csmux != NULL) {
            gdb_use_csmux();
            g_gdb_poll = (int (*)(void))elf_resolvefunction("gdb_poll_breakin");
            if (g_gdb_poll != NULL && g_ops.resolved_tasks && g_ops.task_yield != NULL) {
                systest_id_t pid = 0;
                if (g_ops.task_create("gdb_pump", SYSTEST_PERM_KERNEL, &pid) == CS_OK)
                    g_ops.task_start(pid, (void *)gdb_pump_task, NULL);
            }
        }
    }

    // Normal (non-death) tests: run on boot 0 (or any non-harness run). On a
    // harness resume boot (cursor > 0) they already ran and were reported on
    // boot 0, so skip straight to the death phase.
    if (!harness || cursor == 0) {
        int failed_tests = run_normal_phase(harness, death_count);
        if (harness) {
            systest_ctrl_kv("NORMALDONE fails=", failed_tests); // harness owns the verdict
        } else {
            if (failed_tests == 0)
                DEBUG_PRINT("[SysTest] ALL TESTS PASSED\r\n");
            else
                DEBUG_PRINT("[SysTest] TESTS FAILED\r\n");
            test_platform_exit(failed_tests == 0 ? 0 : 1);
        }
    }

    // ---- death phase (harness only; one death test per boot) ----
    if (cursor < death_count) {
        test_entry_t *e = death_test_at(cursor);
        systest_ctrl_begin(cursor, e->def.suite, e->def.name, e->def.expect_vector);
        systest_death_install_hooks();
        systest_death_arm(e->def.expect_vector, cursor);

        struct test_ctx ctx = {.suite = e->def.suite, .name = e->def.name, .core = 0};
        e->def.fn(&ctx); // lethal: a correct death test does not return here

        // Reached only if the op failed to kill the kernel: that is a failure.
        systest_death_disarm();
        systest_ctrl_kv("SURVIVED cursor=", cursor);
        system_reset(); // harness records the survive as FAIL and advances
    }

    // cursor >= death_count: every death test has been processed.
    systest_ctrl_send("ALLDONE");
    test_platform_exit(0); // harness owns the aggregate pass/fail verdict
    return 0;              // unreachable
}

// ---- registration of SysTest's own framework self-tests --------------------
void systest_register_selftests(void); // suites.c

int module_init(void) {
    CardinalBootInfo *bi = GetBootInfo();
    g_test_mode = (bi != NULL) && (strstr(bi->Cmdline, "cardinal.test") != NULL);
    // "cardinal.harness" additionally enables the CSMUX-driven death-test flow
    // (local, harness-driven runs only). Never set in plain CI runs.
    systest_death_set_harness_mode(
        (bi != NULL) && (strstr(bi->Cmdline, "cardinal.harness") != NULL));

    DEBUG_PRINT("[SysTest] loaded");
    DEBUG_PRINT(g_test_mode ? " (TEST MODE active)\r\n" : "\r\n");

    systest_register_selftests();
    return 0;
}
