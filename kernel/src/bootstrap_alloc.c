/**
 * Copyright (c) 2017 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

#include "elf.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <types.h>

#include <cardinal/local_spinlock.h>

#define BOOTSTRAP_ALLOC_AREA_SIZE (MiB(128))

static ALIGNED(KiB(4)) uint8_t bootstrap_alloc_area[BOOTSTRAP_ALLOC_AREA_SIZE];
static uint64_t bootstrap_alloc_pos = 0;
static int bootstrap_alloc_lock = 0;

void *bootstrap_malloc(size_t s)
{

    void *mem = NULL;

    if (s > 0)
    {

        if (s % 16 != 0) // Require all allocations to be 16-byte aligned
            s = ((s >> 4) + 1) << 4;

        local_spinlock_lock(&bootstrap_alloc_lock);
        if (bootstrap_alloc_pos + s < BOOTSTRAP_ALLOC_AREA_SIZE)
        {
            mem = &bootstrap_alloc_area[bootstrap_alloc_pos];
            bootstrap_alloc_pos += s;
        }
        local_spinlock_unlock(&bootstrap_alloc_lock);
    }
    return mem;
}

void bootstrap_free(void *mem, size_t s)
{
    // If another allocation has not been made yet, we can free the memory
    if (mem == NULL)
        return;

    if (s == 0)
        return;

    if (s % 16 != 0) // Require all allocations to be 16-byte aligned
        s = ((s >> 4) + 1) << 4;

    local_spinlock_lock(&bootstrap_alloc_lock);
    if (bootstrap_alloc_pos > s &&
        &bootstrap_alloc_area[bootstrap_alloc_pos - s] == (uint8_t *)mem)
        bootstrap_alloc_pos -= s;

    local_spinlock_unlock(&bootstrap_alloc_lock);
}
void *(*malloc_hndl)(size_t) = NULL;
void (*free_hndl)(void *) = NULL;

void *WEAK malloc(size_t size)
{

    if (malloc_hndl != NULL)
        return malloc_hndl(size);

    if (size == 0)
        return NULL;

    uint8_t *res = bootstrap_malloc(size + 16);
    if (res == NULL)
        return res;

    memcpy(res, &size, sizeof(size_t));

    return res + 16;
}

void WEAK free(void *ptr)
{
    if (free_hndl != NULL)
    {
        free_hndl(ptr);
        return;
    }

    if (ptr == NULL)
        return;

    size_t sz = 0;
    memcpy(&sz, (uint8_t *)ptr - 16, sizeof(size_t));

    bootstrap_free(ptr, sz + 16);
}

void *WEAK realloc(void *ptr, size_t size)
{
    ptr = NULL;
    size = 0;

    PANIC("realloc unimplemented!");
    return NULL;
}

int kernel_free_avl_bootstrap()
{
    void (*pagealloc_free_hndl)(uintptr_t, uint64_t) = (void (*)(uintptr_t, uint64_t))elf_resolvefunction("pagealloc_free");
    int (*vmem_virttophys_hndl)(void *, intptr_t, intptr_t *) = (int (*)(void *, intptr_t, intptr_t *))elf_resolvefunction("vmem_virttophys");

    local_spinlock_lock(&bootstrap_alloc_lock);

    uintptr_t cur_ptr = (uintptr_t)bootstrap_alloc_pos;
    if (cur_ptr % KiB(4))
        cur_ptr += KiB(4) - (cur_ptr % KiB(4));

    uint64_t rem_sz = BOOTSTRAP_ALLOC_AREA_SIZE - cur_ptr;

    intptr_t phys_addr = 0;
    vmem_virttophys_hndl(NULL, (intptr_t)&bootstrap_alloc_area[cur_ptr], &phys_addr);

    pagealloc_free_hndl((uintptr_t)phys_addr, rem_sz);
    return 0;
}

int kernel_updatememhandlers()
{
    malloc_hndl = (void *(*)(size_t))elf_resolvefunction("malloc");
    if (malloc_hndl == malloc)
        malloc_hndl = NULL;

    free_hndl = (void (*)(void *))elf_resolvefunction("free");
    if (free_hndl == free)
        free_hndl = NULL;

    return 0;
}