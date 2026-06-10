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
    physmem_alloc_flags_zero = (1 << 4),
    physmem_alloc_flags_32bit = (1 << 5),
} physmem_alloc_flags_t;

// Value returned by pagealloc_alloc when the request cannot be satisfied
// (out of memory or too fragmented). Physical address 0 is a legal allocation,
// so the all-ones address is used as the failure sentinel. Callers MUST check
// the return against this before using it. Note for callers that narrow the
// result to 32 bits (physmem_alloc_flags_32bit): check the full uintptr_t
// return against PHYSMEM_NO_ALLOC *before* truncating.
#define PHYSMEM_NO_ALLOC ((uintptr_t)-1)

uintptr_t pagealloc_alloc(int domain, int color, physmem_alloc_flags_t flags, uint64_t size);

void pagealloc_free(uintptr_t addr, uint64_t size);


#endif