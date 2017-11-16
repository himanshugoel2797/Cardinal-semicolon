// Copyright (c) 2017 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINAL_PHYS_MEM_H
#define CARDINAL_PHYS_MEM_H

#include <stdint.h>

typedef enum {
    physmem_alloc_flags_reclaimable = (1 << 0),
    physmem_alloc_flags_data = (1 << 1),
    physmem_alloc_flags_instr = (1 << 2),
    physmem_alloc_flags_pagetable = (1 << 3),
} physmem_alloc_flags_t;

uintptr_t pagealloc_alloc(int domain, int color, physmem_alloc_flags_t flags, uint64_t size);

void pagealloc_free(uint64_t addr, uint64_t size);


#endif