// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#include <stdint.h>
#include <stdbool.h>
#include <cardinal/cs_error.h>

#include "SysTest/test.h"
#include "SysInterrupts/interrupts.h"

static void dummy_handler(int irq) {
    (void)irq;
}

static void test_allocate(test_ctx_t *ctx) {
    int base = 0;
    cs_error err = interrupt_allocate(4, interrupt_flags_none, &base);
    TEST_CHECK_EQ_U(ctx, err, CS_OK);
    TEST_CHECK(ctx, base >= 0);
}

static void test_cpu_idx_inline(test_ctx_t *ctx) {
    TEST_CHECK(ctx, interrupt_get_cpu_idx() >= 0);
}

static void test_cpu_idx_percpu(test_ctx_t *ctx) {
    TEST_CHECK(ctx, interrupt_get_cpu_idx() >= 0);
}

static void test_register_unregister(test_ctx_t *ctx) {
    int base = 0;
    cs_error err = interrupt_allocate(1, interrupt_flags_none, &base);
    TEST_CHECK_EQ_U(ctx, err, CS_OK);
    TEST_CHECK(ctx, base >= 0);
    if (err != CS_OK || base < 0)
        return;

    // Smoke only: register then unregister a dummy handler, never triggered.
    interrupt_register_handler(base, dummy_handler);
    interrupt_unregister_handler(base, dummy_handler);
}

void sysinterrupts_register_tests(void) {
    if (!test_mode_active())
        return;

    test_def_t allocate = {
        .suite = "SysInterrupts",
        .name = "allocate",
        .fn = test_allocate,
        .run = TEST_RUN_INLINE,
        .flags = TEST_FLAG_NONE,
    };
    test_register(&allocate);

    test_def_t cpu_idx_inline = {
        .suite = "SysInterrupts",
        .name = "cpu_idx_inline",
        .fn = test_cpu_idx_inline,
        .run = TEST_RUN_INLINE,
        .flags = TEST_FLAG_NONE,
    };
    test_register(&cpu_idx_inline);

    test_def_t cpu_idx_percpu = {
        .suite = "SysInterrupts",
        .name = "cpu_idx_percpu",
        .fn = test_cpu_idx_percpu,
        .run = TEST_RUN_PERCPU,
        .flags = TEST_FLAG_NONE,
    };
    test_register(&cpu_idx_percpu);

    test_def_t register_unregister = {
        .suite = "SysInterrupts",
        .name = "register_unregister",
        .fn = test_register_unregister,
        .run = TEST_RUN_INLINE,
        .flags = TEST_FLAG_NONE,
    };
    test_register(&register_unregister);
}
