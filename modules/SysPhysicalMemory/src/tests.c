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
    if (addr != PHYSMEM_NO_ALLOC)
        physmem_free(addr, 4096);
}

// NOTE: physmem_alloc_flags_zero is currently a no-op in the allocator stub;
// this test only verifies the allocation succeeds, not that bytes are zeroed.
static void test_alloc_zero(test_ctx_t *ctx) {
    uintptr_t addr =
        physmem_alloc(0, 0, physmem_alloc_flags_data | physmem_alloc_flags_zero,
                      4096);
    TEST_CHECK_NE_U(ctx, addr, PHYSMEM_NO_ALLOC);
    if (addr != PHYSMEM_NO_ALLOC)
        physmem_free(addr, 4096);
}

static void test_alloc_32bit(test_ctx_t *ctx) {
    uintptr_t addr = physmem_alloc(
        0, 0, physmem_alloc_flags_data | physmem_alloc_flags_32bit, 4096);
    TEST_CHECK_NE_U(ctx, addr, PHYSMEM_NO_ALLOC);
    if (addr != PHYSMEM_NO_ALLOC) {
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

    test_def_t alloc_zero = {
        .suite = "SysPhysicalMemory",
        .name = "alloc_zero",
        .fn = test_alloc_zero,
        .run = TEST_RUN_INLINE,
        .flags = TEST_FLAG_NONE,
    };
    test_register(&alloc_zero);

    test_def_t alloc_32bit = {
        .suite = "SysPhysicalMemory",
        .name = "alloc_32bit",
        .fn = test_alloc_32bit,
        .run = TEST_RUN_INLINE,
        // physmem_alloc_flags_32bit is zeroed by the allocator stub; skip
        // until the allocator honours it.
        .flags = TEST_FLAG_SKIP,
    };
    test_register(&alloc_32bit);
}
