// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

#include <types.h>

#include "SysTest/test.h"
#include "SysTaskMgr/task.h"
#include "SysTimer/timer.h"
#include "cs_syscall.h"
#include "error.h"

// SysTest test suite for SysTaskMgr. Registered from module_init (the def is
// copied, so stack temporaries are fine) but only executed when the kernel is
// booted with the "cardinal.test" cmdline flag.

// --- task_corecount ----------------------------------------------------------

static void test_corecount(test_ctx_t *ctx) {
    // At least the BSP must have joined the scheduler by the time tests run.
    TEST_CHECK(ctx, task_corecount() >= 1);
}

// --- semaphore signal/wait across tasks --------------------------------------

// Shared state between the waiter (this task) and the signaller (the sub-task).
// The signaller publishes `flag` BEFORE signalling so that, if semaphore_wait
// genuinely blocked until the signal, the waiter is guaranteed to observe the
// flag set once wait returns. `volatile` keeps the compiler from caching it
// across the wait; ordering is enforced by the semaphore itself.
typedef struct {
    semaphore_t sem;
    volatile int flag;
} sem_test_state_t;

static void sem_signaller(void *arg) {
    sem_test_state_t *s = (sem_test_state_t *)arg;
    s->flag = 1;             // publish before signalling
    semaphore_signal(&s->sem);
}

static void test_semaphore_cross_task(test_ctx_t *ctx) {
    sem_test_state_t s;
    semaphore_init(&s.sem);
    s.flag = 0;

    cs_id sub = 0;
    cs_error err = task_create_kernel("test_sem_sig", task_permissions_kernel, &sub);
    TEST_CHECK_EQ_U(ctx, err, CS_OK);
    if (err != CS_OK)
        return;

    err = task_start_kernel(sub, (void *)sem_signaller, &s);
    TEST_CHECK_EQ_U(ctx, err, CS_OK);
    if (err != CS_OK)
        return;

    // Blocks until the sub-task signals. Correctness, not just liveness:
    semaphore_wait(&s.sem);

    // The flag the signaller set BEFORE signalling must now be visible -- proof
    // the wait actually blocked until the signal rather than racing ahead.
    TEST_CHECK_MSG(ctx, s.flag == 1,
                   "semaphore_wait returned without the signal having landed");
    // The signal must have been consumed by the wait: the count is back to 0.
    TEST_CHECK_EQ_U(ctx, s.sem.count, 0);
}

// --- task_current / task_sleep -----------------------------------------------

static void test_current_and_sleep(test_ctx_t *ctx) {
    cs_id self = task_current();
    TEST_CHECK(ctx, self != 0);

    // Sleep a real interval and confirm wall-clock time actually advanced by at
    // least the requested amount -- proof task_sleep suspended rather than
    // returning immediately. Skip the timing assert only if this platform has
    // no readable counter (timer_timestamp_ns() == TIMER_NO_COUNTER).
    const uint64_t want = MS(5);
    uint64_t t0 = timer_timestamp_ns();

    cs_error err = task_sleep(self, want);
    TEST_CHECK_EQ_U(ctx, err, CS_OK);

    uint64_t t1 = timer_timestamp_ns();
    if (t0 != TIMER_NO_COUNTER && t1 != TIMER_NO_COUNTER) {
        TEST_CHECK_MSG(ctx, (t1 - t0) >= want,
                       "task_sleep returned before the requested interval elapsed");
    }
}

// --- per-core task_current ---------------------------------------------------

// Each core records the id of its own pinned per-cpu task here, indexed by core.
// Task ids are globally unique, so two cores must never see the same id: each
// core checks its id against every already-recorded other core's id. Cores may
// populate this concurrently, but a core only compares against entries that are
// already non-zero, and since ids are unique any populated pair must differ --
// so the check is correct regardless of interleaving.
#define MAX_TEST_CORES 256
static volatile cs_id percpu_ids[MAX_TEST_CORES];

static void test_percpu_current(test_ctx_t *ctx) {
    // Runs once on every online core, each on its own pinned task; every one of
    // them must have a valid current-task id.
    cs_id self = task_current();
    TEST_CHECK_MSG(ctx, self != 0, "task_current() == 0 on a core");

    // Two consecutive reads on the same core must be stable (we are pinned and
    // not blocking between them).
    TEST_CHECK_MSG(ctx, task_current() == self,
                   "task_current() not stable within one core");

    int core = test_core(ctx);
    if (core >= 0 && core < MAX_TEST_CORES) {
        percpu_ids[core] = self;
        // Our per-cpu task id must be distinct from every other core's.
        for (int i = 0; i < MAX_TEST_CORES; i++) {
            if (i == core)
                continue;
            cs_id other = percpu_ids[i];
            TEST_CHECK_MSG(ctx, other == 0 || other != self,
                           "two cores report the same current-task id");
        }
    }
}

// --- registration ------------------------------------------------------------

void systaskmgr_register_tests(void) {
    if (!test_mode_active())
        return;

    test_def_t corecount = {
        .suite = "SysTaskMgr",
        .name = "corecount_at_least_one",
        .fn = test_corecount,
        .run = TEST_RUN_INLINE,
        .flags = TEST_FLAG_NONE,
    };
    test_register(&corecount);

    test_def_t sem = {
        .suite = "SysTaskMgr",
        .name = "semaphore_cross_task",
        .fn = test_semaphore_cross_task,
        .run = TEST_RUN_TASK,
        .flags = TEST_FLAG_NONE,
    };
    test_register(&sem);

    test_def_t cur = {
        .suite = "SysTaskMgr",
        .name = "current_and_sleep",
        .fn = test_current_and_sleep,
        .run = TEST_RUN_TASK,
        .flags = TEST_FLAG_NONE,
    };
    test_register(&cur);

    test_def_t percpu = {
        .suite = "SysTaskMgr",
        .name = "percpu_current",
        .fn = test_percpu_current,
        .run = TEST_RUN_PERCPU,
        .flags = TEST_FLAG_NONE,
    };
    test_register(&percpu);
}
