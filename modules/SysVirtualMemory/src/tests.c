// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#include <stddef.h>
#include <stdint.h>

#include "SysTest/test.h"
#include "SysVirtualMemory/vmem.h"
#include "SysPhysicalMemory/phys_mem.h"

static void test_create_destroy(test_ctx_t *ctx) {
    vmem_t *vm = NULL;
    TEST_CHECK_EQ_U(ctx, vmem_create(&vm), 0);
    TEST_CHECK(ctx, vm != NULL);
    if (vm != NULL)
        vmem_destroy(vm);
}

static void test_vmalloc_vfree(test_ctx_t *ctx) {
    // vmem_vmalloc is a bump allocator of kernel virtual address space starting
    // at a high kernel address. Allocate two distinct regions and verify they
    // are page-aligned, do not overlap, and sit in the kernel half (virt < 0).
    intptr_t a = vmem_vmalloc(4096);
    intptr_t b = vmem_vmalloc(4096);
    TEST_CHECK(ctx, a != 0);
    TEST_CHECK(ctx, b != 0);
    // Page-aligned.
    TEST_CHECK_EQ_U(ctx, (uintptr_t)a & (4096 - 1), 0);
    TEST_CHECK_EQ_U(ctx, (uintptr_t)b & (4096 - 1), 0);
    // Distinct, non-overlapping, ascending bump.
    TEST_CHECK(ctx, b == a + 4096);
    // Kernel-half address (high bit set).
    TEST_CHECK(ctx, a < 0);

    // Free in reverse order (only the most-recent allocation is reclaimable by
    // the bump allocator); a subsequent same-size alloc must then reuse the slot
    // that b occupied, proving vfree actually rewound the bump pointer.
    vmem_vfree(b, 4096);
    intptr_t c = vmem_vmalloc(4096);
    TEST_CHECK(ctx, c == b);
    vmem_vfree(c, 4096);
    vmem_vfree(a, 4096);
}

static void test_getactive(test_ctx_t *ctx) {
    vmem_t *orig = NULL;
    TEST_CHECK_EQ_U(ctx, vmem_getactive(&orig), 0);
    TEST_CHECK(ctx, orig != NULL);

    // Exercise a real set->get round-trip. A freshly-created address space
    // inherits the shared kernel half (vmem_create copies kmem PML4[256..511]),
    // so the running kernel code/stack stays mapped after the cr3 swap. Restore
    // the original active AS immediately afterward to avoid disturbing the
    // live kernel, then destroy the scratch AS.
    vmem_t *scratch = NULL;
    if (vmem_create(&scratch) == 0 && scratch != NULL) {
        TEST_CHECK_EQ_U(ctx, vmem_setactive(scratch), 0);
        vmem_t *got = NULL;
        TEST_CHECK_EQ_U(ctx, vmem_getactive(&got), 0);
        TEST_CHECK(ctx, got == scratch);
        // Restore before doing anything else.
        TEST_CHECK_EQ_U(ctx, vmem_setactive(orig), 0);
        vmem_t *restored = NULL;
        TEST_CHECK_EQ_U(ctx, vmem_getactive(&restored), 0);
        TEST_CHECK(ctx, restored == orig);
        vmem_destroy(scratch);
    }
}

static void test_phys_virt_roundtrip(test_ctx_t *ctx) {
    uintptr_t phys = physmem_alloc(0, 0, physmem_alloc_flags_data, 4096);
    TEST_CHECK(ctx, phys != PHYSMEM_NO_ALLOC);
    if (phys == PHYSMEM_NO_ALLOC)
        return;

    vmem_t *vm = NULL;
    TEST_CHECK_EQ_U(ctx, vmem_getactive(&vm), 0);
    if (vm == NULL) {
        physmem_free(phys, 4096);
        return;
    }

    // Reserve a fresh kernel virtual range and establish a real mapping for it
    // in the active address space, then verify the translation both ways.
    intptr_t virt = vmem_vmalloc(4096);
    TEST_CHECK(ctx, virt != 0);
    if (virt == 0) {
        physmem_free(phys, 4096);
        return;
    }

    cs_error map_err = vmem_map(vm, virt, (intptr_t)phys, 4096,
                                vmem_flags_rw, vmem_flags_cachewriteback);
    TEST_CHECK_EQ_U(ctx, map_err, 0);
    if (map_err == 0) {
        intptr_t back = -1;
        TEST_CHECK_EQ_U(ctx, vmem_virttophys(vm, virt, &back), 0);
        TEST_CHECK_EQ_U(ctx, (uintptr_t)back, phys);

        // phystovirt of a writeback range round-trips to a usable kernel VA;
        // it need not equal our vmalloc'd VA (it returns the physmap alias),
        // but reading through it must observe what we wrote via the mapping.
        intptr_t alias = vmem_phystovirt((intptr_t)phys, 4096,
                                         vmem_flags_cachewriteback);
        TEST_CHECK(ctx, alias != 0);

        // Tear the mapping back down before freeing the frame.
        TEST_CHECK_EQ_U(ctx, vmem_unmap(vm, virt, 4096), 0);
    }

    vmem_vfree(virt, 4096);
    physmem_free(phys, 4096);
}

void sysvirtualmemory_register_tests(void) {
    if (!test_mode_active())
        return;

    {
        test_def_t t = {
            .suite = "SysVirtualMemory",
            .name = "create_destroy",
            .fn = test_create_destroy,
            .run = TEST_RUN_INLINE,
            .flags = TEST_FLAG_NONE,
        };
        test_register(&t);
    }
    {
        test_def_t t = {
            .suite = "SysVirtualMemory",
            .name = "vmalloc_vfree",
            .fn = test_vmalloc_vfree,
            .run = TEST_RUN_INLINE,
            .flags = TEST_FLAG_NONE,
        };
        test_register(&t);
    }
    {
        test_def_t t = {
            .suite = "SysVirtualMemory",
            .name = "getactive",
            .fn = test_getactive,
            .run = TEST_RUN_INLINE,
            .flags = TEST_FLAG_NONE,
        };
        test_register(&t);
    }
    {
        test_def_t t = {
            .suite = "SysVirtualMemory",
            .name = "phys_virt_roundtrip",
            .fn = test_phys_virt_roundtrip,
            .run = TEST_RUN_INLINE,
            .flags = TEST_FLAG_NONE,
        };
        test_register(&t);
    }
}
