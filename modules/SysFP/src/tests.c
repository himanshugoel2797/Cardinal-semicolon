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

// The default-state buffer is an FXSAVE/XSAVE legacy area. Per the SDM and the
// SysFP platform initializer, the default control words are:
//   FCW   (FPU control word) -- 16 bits at byte offset 0   == 0x33F
//   MXCSR (SSE control/status) -- 32 bits at byte offset 24 == 0x1F80
#define FP_DEFAULT_FCW   0x033Fu
#define FP_DEFAULT_MXCSR 0x1F80u
#define FP_MXCSR_OFFSET  24

// The FXSAVE area is always at least 512 bytes; XSAVE is larger still.
#define FP_MIN_STATESIZE 512

// INLINE: the FP-state buffer must be at least the minimum the FXSAVE/XSAVE
// instructions require (>= 512 bytes), not merely positive.
static void test_statesize_positive(test_ctx_t *ctx) {
    int size = fp_platform_getstatesize();
    TEST_CHECK_MSG(ctx, size >= FP_MIN_STATESIZE,
                   "FP state size is at least the 512-byte FXSAVE minimum");
}

// INLINE: the FXSAVE/XSAVE area alignment must be a power of two and one of the
// two valid values (16 for FXSAVE, 64 for XSAVE).
static void test_align_positive(test_ctx_t *ctx) {
    int align = fp_platform_getalign();
    TEST_CHECK(ctx, align > 0);
    // Power of two.
    TEST_CHECK_MSG(ctx, (align & (align - 1)) == 0,
                   "FP state alignment is a power of two");
    // Exactly the FXSAVE (16) or XSAVE (64) requirement.
    TEST_CHECK_MSG(ctx, align == 16 || align == 64,
                   "FP state alignment is 16 (FXSAVE) or 64 (XSAVE)");
}

// INLINE: run the default-state initializer on a correctly aligned buffer and
// verify it produced the architectural default control words. Over-allocate by
// the alignment, round the pointer up. Does not touch live FP state.
static void test_getdefaultstate_smoke(test_ctx_t *ctx) {
    int size = fp_platform_getstatesize();
    int align = fp_platform_getalign();
    TEST_CHECK(ctx, size >= FP_MIN_STATESIZE);
    TEST_CHECK(ctx, align > 0);
    if (size < FP_MIN_STATESIZE || align <= 0)
        return;

    uint8_t *raw = (uint8_t *)malloc((size_t)size + (size_t)align);
    TEST_CHECK(ctx, raw != NULL);
    if (raw == NULL)
        return;

    uintptr_t addr = (uintptr_t)raw;
    uintptr_t aligned = (addr + (uintptr_t)align - 1) & ~((uintptr_t)align - 1);
    TEST_CHECK(ctx, (aligned % (uintptr_t)align) == 0);

    // Poison the control-word slots first so we know the initializer wrote them.
    uint8_t *buf = (uint8_t *)aligned;
    buf[0] = 0xFF;
    buf[1] = 0xFF;
    for (int i = 0; i < 4; i++)
        buf[FP_MXCSR_OFFSET + i] = 0xFF;

    fp_platform_getdefaultstate((void *)aligned);

    // FCW: 16-bit LE field at offset 0.
    uint32_t fcw = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8);
    TEST_CHECK_EQ_U(ctx, fcw, FP_DEFAULT_FCW);

    // MXCSR: 32-bit LE field at offset 24.
    uint32_t mxcsr = (uint32_t)buf[FP_MXCSR_OFFSET] |
                     ((uint32_t)buf[FP_MXCSR_OFFSET + 1] << 8) |
                     ((uint32_t)buf[FP_MXCSR_OFFSET + 2] << 16) |
                     ((uint32_t)buf[FP_MXCSR_OFFSET + 3] << 24);
    TEST_CHECK_EQ_U(ctx, mxcsr, FP_DEFAULT_MXCSR);

    free(raw);
}

// PERCPU: mirror SMP bring-up by re-running the per-AP FP init on every online
// core, then assert the platform FP-state descriptors (size/alignment) are sane
// and identical on every core. The descriptors are derived from CPUID/XSAVE
// feature state that must be uniform across the package, so any core reporting a
// different size/align would be a real SMP-consistency bug.
//
// The first core to run latches the reference values into static volatile slots;
// every later core compares against them. Reads/writes of the (naturally
// aligned, word-sized) slots are atomic on x86_64, and TEST_RUN_PERCPU runs one
// pinned task per core sequentially enough that the latch is visible -- but we
// still gate the latch on a "captured" flag so only the first core writes.
static volatile int g_fp_ref_captured = 0;
static volatile int g_fp_ref_size = 0;
static volatile int g_fp_ref_align = 0;

static void test_mp_init_consistency(test_ctx_t *ctx) {
    // Re-run per-AP FP init on this core, as the SMP bring-up does via
    // apscript.txt. Must succeed (returns 0).
    int rc = fp_mp_init();
    TEST_CHECK_EQ_U(ctx, (uint64_t)rc, 0);

    int size = fp_platform_getstatesize();
    int align = fp_platform_getalign();

    // Sanity: same invariants the INLINE tests assert, re-verified per core.
    TEST_CHECK_MSG(ctx, size >= FP_MIN_STATESIZE,
                   "per-core FP state size is at least the 512-byte minimum");
    TEST_CHECK_MSG(ctx, align == 16 || align == 64,
                   "per-core FP state alignment is 16 (FXSAVE) or 64 (XSAVE)");

    if (!g_fp_ref_captured) {
        // First core: latch the reference values.
        g_fp_ref_size = size;
        g_fp_ref_align = align;
        g_fp_ref_captured = 1;
    } else {
        // Later cores: must agree with the first core's values.
        TEST_CHECK_EQ_U(ctx, (uint64_t)size, (uint64_t)g_fp_ref_size);
        TEST_CHECK_EQ_U(ctx, (uint64_t)align, (uint64_t)g_fp_ref_align);
    }
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

    test_def_t mpinit = {
        .suite = "SysFP", .name = "mp_init_consistency",
        .fn = test_mp_init_consistency,
        .run = TEST_RUN_PERCPU, .flags = TEST_FLAG_NONE,
    };
    test_register(&mpinit);
}
