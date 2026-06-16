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
    intptr_t addr = vmem_vmalloc(4096);
    TEST_CHECK(ctx, addr != 0);
    if (addr != 0)
        vmem_vfree(addr, 4096);
}

static void test_getactive(test_ctx_t *ctx) {
    vmem_t *vm = NULL;
    TEST_CHECK_EQ_U(ctx, vmem_getactive(&vm), 0);
    TEST_CHECK(ctx, vm != NULL);
}

static void test_phys_virt_roundtrip(test_ctx_t *ctx) {
    uintptr_t phys = physmem_alloc(0, 0, physmem_alloc_flags_data, 4096);
    TEST_CHECK(ctx, phys != PHYSMEM_NO_ALLOC);
    if (phys == PHYSMEM_NO_ALLOC)
        return;

    intptr_t virt = vmem_phystovirt((intptr_t)phys, 4096, vmem_flags_rw);
    TEST_CHECK(ctx, virt != 0);

    if (virt != 0) {
        vmem_t *vm = NULL;
        TEST_CHECK_EQ_U(ctx, vmem_getactive(&vm), 0);
        if (vm != NULL) {
            intptr_t back = 0;
            TEST_CHECK_EQ_U(ctx, vmem_virttophys(vm, virt, &back), 0);
            TEST_CHECK_EQ_U(ctx, (uintptr_t)back, phys);
        }
    }

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
