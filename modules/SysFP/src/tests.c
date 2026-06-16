// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT
//
// SysTest tests for SysFP. These validate the platform FP-state descriptors
// (size/alignment) and that the default-state initializer runs without
// faulting on a properly aligned buffer. They deliberately do NOT call
// fp_platform_getstate/setstate -- mutating live FP state outside a full
// task-context save/restore is unsafe.

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "SysTest/test.h"
#include "SysFP/fp.h"

// INLINE: the FP-state buffer size must be a sane, positive value.
static void test_statesize_positive(test_ctx_t *ctx) {
    int size = fp_platform_getstatesize();
    TEST_CHECK(ctx, size > 0);
}

// INLINE: the required FP-state alignment must be a sane, positive value.
static void test_align_positive(test_ctx_t *ctx) {
    int align = fp_platform_getalign();
    TEST_CHECK(ctx, align > 0);
}

// INLINE: smoke-test the default-state initializer on a correctly aligned
// buffer. Over-allocate by the alignment, round the pointer up, and confirm
// the call returns without faulting. Does not touch live FP state.
static void test_getdefaultstate_smoke(test_ctx_t *ctx) {
    int size = fp_platform_getstatesize();
    int align = fp_platform_getalign();
    TEST_CHECK(ctx, size > 0);
    TEST_CHECK(ctx, align > 0);
    if (size <= 0 || align <= 0)
        return;

    uint8_t *raw = (uint8_t *)malloc((size_t)size + (size_t)align);
    TEST_CHECK(ctx, raw != NULL);
    if (raw == NULL)
        return;

    uintptr_t addr = (uintptr_t)raw;
    uintptr_t aligned = (addr + (uintptr_t)align - 1) & ~((uintptr_t)align - 1);
    TEST_CHECK(ctx, (aligned % (uintptr_t)align) == 0);

    fp_platform_getdefaultstate((void *)aligned);

    free(raw);
}

void sysfp_register_tests(void) {
    if (!test_mode_active())
        return;

    test_def_t statesize = {
        .suite = "SysFP", .name = "statesize_positive",
        .fn = test_statesize_positive,
        .run = TEST_RUN_INLINE, .flags = TEST_FLAG_NONE,
    };
    test_register(&statesize);

    test_def_t align = {
        .suite = "SysFP", .name = "align_positive",
        .fn = test_align_positive,
        .run = TEST_RUN_INLINE, .flags = TEST_FLAG_NONE,
    };
    test_register(&align);

    test_def_t defstate = {
        .suite = "SysFP", .name = "getdefaultstate_smoke",
        .fn = test_getdefaultstate_smoke,
        .run = TEST_RUN_INLINE, .flags = TEST_FLAG_NONE,
    };
    test_register(&defstate);
}
