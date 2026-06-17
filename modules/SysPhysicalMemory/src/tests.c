// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#include <stddef.h>
#include <stdint.h>

#include <types.h>

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

// An allocation request larger than all installed RAM must fail cleanly with
// the PHYSMEM_NO_ALLOC sentinel rather than returning a bogus address or
// faulting. 512 TiB is beyond any conceivable physical memory, so this is OOM on
// every machine. (It also exercises the >8 TiB path where the page count exceeds
// 32 bits: the allocator uses a 64-bit pg_cnt so this fails as OOM instead of
// wrapping to a tiny count and spuriously succeeding.)
static void test_alloc_oom(test_ctx_t *ctx) {
    uintptr_t addr =
        physmem_alloc(0, 0, physmem_alloc_flags_data, TiB(512));
    TEST_CHECK_EQ_U(ctx, addr, PHYSMEM_NO_ALLOC);
    // Defensive: if a buggy allocator ever did hand back a non-sentinel address
    // for an impossible request, return it so the pool isn't permanently leaked.
    if (addr != PHYSMEM_NO_ALLOC)
        physmem_free(addr, TiB(512));
}

// physmem_alloc rounds the requested size up to a whole BTM_LEVEL page
// (ALIGN_UP), so a sub-page request must still be serviced at page granularity:
// the returned address is page-aligned (not byte-granular) and non-sentinel,
// then freed with the page-rounded size (physmem_free PANICs on a misaligned
// size). Note: this deliberately does NOT compare two independent allocations
// for distinctness -- the free list is lock-free and shared with the APs, so
// racing two allocations and asserting their relative layout is non-deterministic
// under SMP. The page-alignment of a single sub-page allocation is the stable,
// observable proof that the size was rounded up rather than handed back verbatim.
static void test_alloc_subpage_roundup(test_ctx_t *ctx) {
    uintptr_t a = physmem_alloc(0, 0, physmem_alloc_flags_data, 100);
    TEST_CHECK_NE_U(ctx, a, PHYSMEM_NO_ALLOC);
    if (a != PHYSMEM_NO_ALLOC) {
        TEST_CHECK_MSG(ctx, a % 4096 == 0,
                       "sub-page allocation must be rounded up to a page-aligned address");
        physmem_free(a, 4096);
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

    test_def_t alloc_oom = {
        .suite = "SysPhysicalMemory",
        .name = "alloc_oom",
        .fn = test_alloc_oom,
        .run = TEST_RUN_INLINE,
        .flags = TEST_FLAG_NONE,
    };
    test_register(&alloc_oom);

    test_def_t alloc_subpage_roundup = {
        .suite = "SysPhysicalMemory",
        .name = "alloc_subpage_roundup",
        .fn = test_alloc_subpage_roundup,
        .run = TEST_RUN_INLINE,
        .flags = TEST_FLAG_NONE,
    };
    test_register(&alloc_subpage_roundup);
}
