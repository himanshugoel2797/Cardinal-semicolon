// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT
//
// SysTest's own framework self-tests. These exercise the three execution
// triggers using only kernel/common facilities (so SysTest stays loadable very
// early, with no module dependencies). Tests for a specific subsystem belong in
// that subsystem's module -- it includes <SysTest/test.h> and calls
// test_register() from its module_init.

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <types.h>

#include "SysTest/test.h"

// INLINE: pure logic + heap round-trip, runs in the runner thread.
static void selftest_inline(test_ctx_t *ctx) {
    TEST_CHECK(ctx, 1 + 1 == 2);
    TEST_CHECK_EQ_U(ctx, 0xFFu & 0x0Fu, 0x0Fu);

    uint8_t *buf = (uint8_t *)malloc(64);
    TEST_CHECK(ctx, buf != NULL);
    if (buf != NULL) {
        memset(buf, 0xAB, 64);
        TEST_CHECK_EQ_U(ctx, buf[0], 0xAB);
        TEST_CHECK_EQ_U(ctx, buf[63], 0xAB);
        free(buf);
    }
}

// TASK: runs in its own kernel task. Proves the task-context path works.
static void selftest_task(test_ctx_t *ctx) {
    // In a TEST_RUN_TASK test the runner descheduled to wait for us, so simply
    // reaching here proves the spawn/join path. Do a little heap work too.
    void *p = malloc(128);
    TEST_CHECK(ctx, p != NULL);
    free(p);
    TEST_CHECK_EQ_U(ctx, test_core(ctx), 0); // non-pinned: core field is 0
}

// PERCPU: runs once on every online core. The runner aggregates per-core results.
static void selftest_percpu(test_ctx_t *ctx) {
    int core = test_core(ctx);
    TEST_CHECK(ctx, core >= 0);
    TEST_CHECK(ctx, core < 256);
}

// ---- death tests: each MUST kill the kernel (harness-driven local runs only) -
//
// A death test's body is expected not to return: it triggers a specific CPU
// fault (or a PANIC) and the death-test machinery confirms the kernel died with
// the expected vector, then reboots so the harness can advance to the next one.
// These double as a smoke test of the fault/PANIC paths themselves.

// #GP (vector 13): a write to a non-canonical linear address raises #GP(0).
static void death_gp_noncanonical(test_ctx_t *ctx) {
    (void)ctx;
    *(volatile uint64_t *)0xDEADBEEFDEADBEE0ull = 0;
}

// #UD (vector 6): an undefined opcode.
static void death_ud_invalidop(test_ctx_t *ctx) {
    (void)ctx;
    __asm__ volatile("ud2");
}

// PANIC path (no CPU vector): an explicit kernel panic. Expected outcome "any".
static void death_panic(test_ctx_t *ctx) {
    (void)ctx;
    PANIC("SysTest death-test: intentional panic");
}

void systest_register_selftests(void) {
    test_def_t inl = {
        .suite = "SysTest", .name = "selftest_inline", .fn = selftest_inline,
        .run = TEST_RUN_INLINE, .flags = TEST_FLAG_NONE,
    };
    test_register(&inl);

    test_def_t tsk = {
        .suite = "SysTest", .name = "selftest_task", .fn = selftest_task,
        .run = TEST_RUN_TASK, .flags = TEST_FLAG_NONE,
    };
    test_register(&tsk);

    test_def_t pcpu = {
        .suite = "SysTest", .name = "selftest_percpu", .fn = selftest_percpu,
        .run = TEST_RUN_PERCPU, .flags = TEST_FLAG_NONE,
    };
    test_register(&pcpu);

    // Death tests (run only under a harness-driven local run; SKIP otherwise).
    test_def_t d_gp = TEST_DEATH_DEF("SysTest", "death_gp_noncanonical",
                                     death_gp_noncanonical, 13 /* #GP */);
    test_register(&d_gp);

    test_def_t d_ud = TEST_DEATH_DEF("SysTest", "death_ud_invalidop",
                                     death_ud_invalidop, 6 /* #UD */);
    test_register(&d_ud);

    test_def_t d_panic = TEST_DEATH_DEF("SysTest", "death_panic",
                                        death_panic, TEST_DEATH_ANY);
    test_register(&d_panic);
}
