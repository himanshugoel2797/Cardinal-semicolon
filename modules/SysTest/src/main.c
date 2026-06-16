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

typedef uint64_t systest_id_t;
#define SYSTEST_PERM_KERNEL 1 // task_permissions_kernel

typedef struct {
    cs_error (*task_create)(const char *, int, systest_id_t *);
    cs_error (*task_create_oncore)(const char *, int, int, systest_id_t *);
    cs_error (*task_start)(systest_id_t, void *, void *);
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

static void print_uint(uint64_t v) {
    char buf[24];
    DEBUG_PRINT(ltoa(v, buf, 10));
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
    if (g_ops.task_create("systest_task", SYSTEST_PERM_KERNEL, &id) != CS_OK ||
            g_ops.task_start(id, (void *)test_task_trampoline, &arg) != CS_OK) {
        ctx.checks++;
        ctx.fails++;
        DEBUG_PRINT("    # FAIL: could not spawn test task\r\n");
    } else {
        g_ops.sem_wait(&sem); // blocks (deschedules) until the task signals
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
        if (g_ops.task_create_oncore("systest_percpu", SYSTEST_PERM_KERNEL, c, &id) != CS_OK ||
                g_ops.task_start(id, (void *)test_task_trampoline, &args[c]) != CS_OK) {
            ctxs[c].checks++;
            ctxs[c].fails++;
            DEBUG_PRINT("    # FAIL: could not spawn per-CPU task on core ");
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
        execute_entry(&n->entry);
        n->entry.done = true;
        fails += report_entry(++idx, &n->entry);
    }
    return (int)fails;
}

int test_run_all(void) {
    if (!g_test_mode)
        return 0; // normal boot: nothing to do, no cost

    resolve_ops();

    DEBUG_PRINT("[SysTest] ============ test run start ============\r\n");
    DEBUG_PRINT("1..");
    print_uint((uint64_t)g_test_count);
    DEBUG_PRINT("\r\n");
    if (!g_ops.resolved_tasks)
        DEBUG_PRINT("[SysTest] WARN: scheduler ops unresolved; task/per-CPU tests run inline\r\n");

    uint32_t total_fail_checks = 0;
    int failed_tests = 0;
    int idx = 0;
    for (test_node_t *n = g_tests_head; n != NULL; n = n->next) {
        test_entry_t *e = &n->entry;
        if (e->done) {
            // Already run via test_run_suite(); still account for it in the plan.
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

    DEBUG_PRINT("[SysTest] ============ ");
    print_uint((uint64_t)g_test_count);
    DEBUG_PRINT(" tests, ");
    print_uint((uint64_t)failed_tests);
    DEBUG_PRINT(" failed, ");
    print_uint((uint64_t)total_fail_checks);
    DEBUG_PRINT(" failed checks ============\r\n");

    if (failed_tests == 0)
        DEBUG_PRINT("[SysTest] ALL TESTS PASSED\r\n");
    else
        DEBUG_PRINT("[SysTest] TESTS FAILED\r\n");

    test_platform_exit(failed_tests == 0 ? 0 : 1);
    return 0; // unreachable
}

// ---- registration of SysTest's own framework self-tests --------------------
void systest_register_selftests(void); // suites.c

int module_init(void) {
    CardinalBootInfo *bi = GetBootInfo();
    g_test_mode = (bi != NULL) && (strstr(bi->Cmdline, "cardinal.test") != NULL);

    DEBUG_PRINT("[SysTest] loaded");
    DEBUG_PRINT(g_test_mode ? " (TEST MODE active)\r\n" : "\r\n");

    systest_register_selftests();
    return 0;
}
