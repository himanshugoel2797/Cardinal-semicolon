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
// faulting. 64 GiB is far beyond any CI/QEMU memory size yet its page count
// (16 Mi pages) still fits the allocator's int32 page counter, so the free-list
// scan terminates and returns the sentinel deterministically.
static void test_alloc_oom(test_ctx_t *ctx) {
    uintptr_t addr =
        physmem_alloc(0, 0, physmem_alloc_flags_data, GiB(64));
    TEST_CHECK_EQ_U(ctx, addr, PHYSMEM_NO_ALLOC);
    // Defensive: if a buggy allocator ever did hand back a non-sentinel address
    // for an impossible request, return it so the pool isn't permanently leaked.
    if (addr != PHYSMEM_NO_ALLOC)
        physmem_free(addr, GiB(64));
}

// physmem_alloc rounds the requested size up to a whole BTM_LEVEL page
// (ALIGN_UP). A sub-page request must therefore still yield a page-aligned
// address and consume a full page: two consecutive sub-page allocations must
// land in distinct pages (>= one page apart), proving the round-up happened and
// the second request did not alias the first. Both are freed with the
// page-rounded size (physmem_free PANICs on a misaligned size).
static void test_alloc_subpage_roundup(test_ctx_t *ctx) {
    uintptr_t a = physmem_alloc(0, 0, physmem_alloc_flags_data, 100);
    TEST_CHECK_NE_U(ctx, a, PHYSMEM_NO_ALLOC);
    uintptr_t b = physmem_alloc(0, 0, physmem_alloc_flags_data, 1);
    TEST_CHECK_NE_U(ctx, b, PHYSMEM_NO_ALLOC);

    if (a != PHYSMEM_NO_ALLOC) {
        TEST_CHECK_MSG(ctx, a % KiB(4) == 0,
                       "sub-page allocation must be page-aligned");
    }
    if (b != PHYSMEM_NO_ALLOC) {
        TEST_CHECK_MSG(ctx, b % KiB(4) == 0,
                       "sub-page allocation must be page-aligned");
    }
    // Distinct pages: a 100-byte and a 1-byte request each took a whole page.
    if (a != PHYSMEM_NO_ALLOC && b != PHYSMEM_NO_ALLOC) {
        uintptr_t lo = a < b ? a : b;
        uintptr_t hi = a < b ? b : a;
        TEST_CHECK_MSG(ctx, hi - lo >= KiB(4),
                       "sub-page allocations must not alias the same page");
    }

    if (a != PHYSMEM_NO_ALLOC)
        physmem_free(a, KiB(4));
    if (b != PHYSMEM_NO_ALLOC)
        physmem_free(b, KiB(4));
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
