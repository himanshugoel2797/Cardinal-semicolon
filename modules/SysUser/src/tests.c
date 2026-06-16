// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#include <stdlib.h>

#include "SysTest/test.h"
#include "SysUser/syscall.h"

// SysTest test suite for SysUser. Registered from module_init; only executed
// when the kernel is booted with the "cardinal.test" cmdline flag.

static void test_getfullstate_size(test_ctx_t *ctx)
{
    // The full-state blob must have a positive size for save/restore to work.
    TEST_CHECK(ctx, syscall_getfullstate_size() > 0);
}

static void test_sethandler_negative(test_ctx_t *ctx)
{
    // A negative handler index is out of range and must be rejected.
    TEST_CHECK(ctx, syscall_sethandler(-1, NULL) != CS_OK);
}

static void test_sethandler_overflow(test_ctx_t *ctx)
{
    // An index at SYSCALL_COUNT is one past the end and must be rejected.
    TEST_CHECK(ctx, syscall_sethandler(SYSCALL_COUNT, NULL) != CS_OK);
}

static void test_sethandler_valid(test_ctx_t *ctx)
{
    // A valid in-range index must succeed.
    TEST_CHECK_EQ_U(ctx, syscall_sethandler(1, NULL), CS_OK);
}

void sysuser_register_tests(void)
{
    if (!test_mode_active())
        return;

    {
        test_def_t t = {
            .suite = "SysUser",
            .name = "getfullstate_size_positive",
            .fn = test_getfullstate_size,
            .run = TEST_RUN_INLINE,
            .flags = TEST_FLAG_NONE,
        };
        test_register(&t);
    }
    {
        test_def_t t = {
            .suite = "SysUser",
            .name = "sethandler_rejects_negative",
            .fn = test_sethandler_negative,
            .run = TEST_RUN_INLINE,
            .flags = TEST_FLAG_NONE,
        };
        test_register(&t);
    }
    {
        test_def_t t = {
            .suite = "SysUser",
            .name = "sethandler_rejects_overflow",
            .fn = test_sethandler_overflow,
            .run = TEST_RUN_INLINE,
            .flags = TEST_FLAG_NONE,
        };
        test_register(&t);
    }
    {
        test_def_t t = {
            .suite = "SysUser",
            .name = "sethandler_accepts_valid",
            .fn = test_sethandler_valid,
            .run = TEST_RUN_INLINE,
            .flags = TEST_FLAG_NONE,
        };
        test_register(&t);
    }
}
