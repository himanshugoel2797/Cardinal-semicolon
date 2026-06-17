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
    // The full-state blob carries at least the syscall-set dispatch table
    // (SYSCALL_SET_COUNT pointers), so its size must clear that lower bound --
    // a plain "> 0" check can never fail and proves nothing.
    int sz = syscall_getfullstate_size();
    TEST_CHECK(ctx, sz >= (int)(SYSCALL_SET_COUNT * sizeof(void *)));
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
    // syscall set 0 IS the syscall_funcs table, so we can read a slot back to
    // prove the store took effect -- and, crucially, save/restore it. The
    // handler table is module-lifetime global state shared with the live
    // syscall path; the old test left NULL in slot 1, which would fault any
    // later dispatch to that index.
    void **set0 = syscall_get_syscallset(0);
    TEST_CHECK_MSG(ctx, set0 != NULL, "syscall set 0 must be present");
    if (set0 == NULL)
        return;

    void *orig = set0[1];

    // A valid in-range index must succeed AND actually store the handler. Use a
    // known non-NULL pointer (this function's own address) as the sentinel.
    void *sentinel = (void *)&test_sethandler_valid;
    TEST_CHECK_EQ_U(ctx, syscall_sethandler(1, sentinel), CS_OK);
    TEST_CHECK_MSG(ctx, set0[1] == sentinel, "handler was not stored");

    // Restore the original handler so the test leaves no destructive side-effect.
    TEST_CHECK_EQ_U(ctx, syscall_sethandler(1, orig), CS_OK);
    TEST_CHECK_MSG(ctx, set0[1] == orig, "original handler not restored");
}

static void test_set_syscallset_negative(test_ctx_t *ctx)
{
    // A negative syscall-set index is out of range and must be rejected
    // without an out-of-bounds write into the dispatch table.
    TEST_CHECK(ctx, syscall_set_syscallset(-1, NULL) != CS_OK);
}

static void test_set_syscallset_overflow(test_ctx_t *ctx)
{
    // An index at SYSCALL_SET_COUNT is one past the end and must be rejected.
    TEST_CHECK(ctx, syscall_set_syscallset(SYSCALL_SET_COUNT, NULL) != CS_OK);
}

static void test_get_syscallset_negative(test_ctx_t *ctx)
{
    // A negative index must be rejected -- returns NULL, no OOB read.
    TEST_CHECK_EQ_PTR(ctx, syscall_get_syscallset(-1), NULL);
}

static void test_get_syscallset_overflow(test_ctx_t *ctx)
{
    // An index at SYSCALL_SET_COUNT is one past the end -- returns NULL.
    TEST_CHECK_EQ_PTR(ctx, syscall_get_syscallset(SYSCALL_SET_COUNT), NULL);
}

static void test_syscallset_roundtrip(test_ctx_t *ctx)
{
    // The syscall_set_table is module-lifetime global state shared with the live
    // syscall path; set 0 holds syscall_funcs. Exercise the round-trip on the
    // last slot (defaults to NULL) and restore it so no live dispatch is harmed.
    int idx = SYSCALL_SET_COUNT - 1;
    void **orig = syscall_get_syscallset(idx);

    // A valid in-range store must succeed and be observable via get.
    void *sentinel = (void *)&test_syscallset_roundtrip;
    TEST_CHECK_EQ_U(ctx, syscall_set_syscallset(idx, (void **)sentinel), CS_OK);
    TEST_CHECK_EQ_PTR(ctx, syscall_get_syscallset(idx), sentinel);

    // Restore the original entry so the test leaves no destructive side-effect.
    TEST_CHECK_EQ_U(ctx, syscall_set_syscallset(idx, orig), CS_OK);
    TEST_CHECK_EQ_PTR(ctx, syscall_get_syscallset(idx), orig);
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
    {
        test_def_t t = {
            .suite = "SysUser",
            .name = "set_syscallset_rejects_negative",
            .fn = test_set_syscallset_negative,
            .run = TEST_RUN_INLINE,
            .flags = TEST_FLAG_NONE,
        };
        test_register(&t);
    }
    {
        test_def_t t = {
            .suite = "SysUser",
            .name = "set_syscallset_rejects_overflow",
            .fn = test_set_syscallset_overflow,
            .run = TEST_RUN_INLINE,
            .flags = TEST_FLAG_NONE,
        };
        test_register(&t);
    }
    {
        test_def_t t = {
            .suite = "SysUser",
            .name = "get_syscallset_rejects_negative",
            .fn = test_get_syscallset_negative,
            .run = TEST_RUN_INLINE,
            .flags = TEST_FLAG_NONE,
        };
        test_register(&t);
    }
    {
        test_def_t t = {
            .suite = "SysUser",
            .name = "get_syscallset_rejects_overflow",
            .fn = test_get_syscallset_overflow,
            .run = TEST_RUN_INLINE,
            .flags = TEST_FLAG_NONE,
        };
        test_register(&t);
    }
    {
        test_def_t t = {
            .suite = "SysUser",
            .name = "syscallset_roundtrip",
            .fn = test_syscallset_roundtrip,
            .run = TEST_RUN_INLINE,
            .flags = TEST_FLAG_NONE,
        };
        test_register(&t);
    }
}
