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

static void sem_signaller(void *arg) {
    semaphore_t *sem = (semaphore_t *)arg;
    semaphore_signal(sem);
}

static void test_semaphore_cross_task(test_ctx_t *ctx) {
    semaphore_t sem;
    semaphore_init(&sem);

    cs_id sub = 0;
    cs_error err = task_create_kernel("test_sem_sig", task_permissions_kernel, &sub);
    TEST_CHECK_EQ_U(ctx, err, CS_OK);
    if (err != CS_OK)
        return;

    err = task_start_kernel(sub, (void *)sem_signaller, &sem);
    TEST_CHECK_EQ_U(ctx, err, CS_OK);
    if (err != CS_OK)
        return;

    // Blocks until the sub-task signals; if signalling were broken this would
    // hang the test task rather than fail, so the very fact we return is the
    // pass condition.
    semaphore_wait(&sem);
    TEST_CHECK(ctx, true);
}

// --- task_current / task_sleep -----------------------------------------------

static void test_current_and_sleep(test_ctx_t *ctx) {
    cs_id self = task_current();
    TEST_CHECK(ctx, self != 0);

    cs_error err = task_sleep(self, US(100));
    TEST_CHECK_EQ_U(ctx, err, CS_OK);
}

// --- per-core task_current ---------------------------------------------------

static void test_percpu_current(test_ctx_t *ctx) {
    // Runs once on every online core, each on its own pinned task; every one of
    // them must have a valid current-task id.
    cs_id self = task_current();
    TEST_CHECK_MSG(ctx, self != 0, "task_current() == 0 on a core");
    (void)test_core(ctx);
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
