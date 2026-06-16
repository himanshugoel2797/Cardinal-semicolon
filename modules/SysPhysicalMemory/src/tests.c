// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#include <stddef.h>
#include <stdint.h>

#include "SysPhysicalMemory/phys_mem.h"
#include "SysTest/test.h"

// PHYSMEM_NO_ALLOC must remain the all-ones sentinel; callers compare the full
// uintptr_t return against it before use/truncation.
_Static_assert(PHYSMEM_NO_ALLOC == (uintptr_t)-1,
               "PHYSMEM_NO_ALLOC must be (uintptr_t)-1");

static void test_alloc_free(test_ctx_t *ctx) {
    uintptr_t addr = physmem_alloc(0, 0, physmem_alloc_flags_data, 4096);
    TEST_CHECK_NE_U(ctx, addr, PHYSMEM_NO_ALLOC);
    if (addr != PHYSMEM_NO_ALLOC) {
        TEST_CHECK_MSG(ctx, addr % 4096 == 0,
                       "allocation must be page-aligned");
        physmem_free(addr, 4096);
    }
}

// test_alloc_zero_flag verifies that physmem_alloc_flags_zero is accepted by
// the allocator without failure and that the returned page is page-aligned.
// NOTE: physmem_alloc currently discards all flag bits (flags = 0 in the
// implementation), so actual zero-content verification is deferred until the
// zeroing path is wired up.  This test honestly documents that contract.
static void test_alloc_zero_flag(test_ctx_t *ctx) {
    uintptr_t addr =
        physmem_alloc(0, 0, physmem_alloc_flags_data | physmem_alloc_flags_zero,
                      4096);
    TEST_CHECK_NE_U(ctx, addr, PHYSMEM_NO_ALLOC);
    if (addr != PHYSMEM_NO_ALLOC) {
        TEST_CHECK_MSG(ctx, addr % 4096 == 0,
                       "zero-flagged allocation must be page-aligned");
        physmem_free(addr, 4096);
    }
}

static void test_alloc_32bit(test_ctx_t *ctx) {
    uintptr_t addr = physmem_alloc(
        0, 0, physmem_alloc_flags_data | physmem_alloc_flags_32bit, 4096);
    TEST_CHECK_NE_U(ctx, addr, PHYSMEM_NO_ALLOC);
    if (addr != PHYSMEM_NO_ALLOC) {
        TEST_CHECK_MSG(ctx, addr % 4096 == 0,
                       "32-bit allocation must be page-aligned");
        TEST_CHECK_MSG(ctx, addr < 0x100000000ULL,
                       "32-bit allocation must be below 4 GiB");
        physmem_free(addr, 4096);
    }
}

void sysphysicalmemory_register_tests(void) {
    if (!test_mode_active())
        return;

    test_def_t alloc_free = {
        .suite = "SysPhysicalMemory",
        .name = "alloc_free",
        .fn = test_alloc_free,
        .run = TEST_RUN_INLINE,
        .flags = TEST_FLAG_NONE,
    };
    test_register(&alloc_free);

    test_def_t alloc_zero_flag = {
        .suite = "SysPhysicalMemory",
        .name = "alloc_zero_flag",
        .fn = test_alloc_zero_flag,
        .run = TEST_RUN_INLINE,
        .flags = TEST_FLAG_NONE,
    };
    test_register(&alloc_zero_flag);

    test_def_t alloc_32bit = {
        .suite = "SysPhysicalMemory",
        .name = "alloc_32bit",
        .fn = test_alloc_32bit,
        .run = TEST_RUN_INLINE,
        .flags = TEST_FLAG_NONE,
    };
    test_register(&alloc_32bit);
}
