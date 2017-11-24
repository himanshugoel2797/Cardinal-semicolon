// Copyright (c) 2017 Himanshu Goel
// 
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINAL_VMEM_H
#define CARDINAL_VMEM_H

    //vmem_map
    //vmem_unmap
    //vmem_create
    //vmem_setactive
    //vmem_virttophys
    //vmem_phystovirt
    //vmem_init

#include <stdint.h>
#include <stddef.h>

typedef struct vmem vmem_t;

typedef enum {
    vmem_flags_read = 0,
    vmem_flags_write = (1 << 0),
    vmem_flags_exec = (1 << 1),
    vmem_flags_cachewritethrough = (1 << 2),
    vmem_flags_cachewriteback = (1 << 3),
    vmem_flags_uncached = (1 << 4),

    vmem_flags_kernel = (1 << 5),
    vmem_flags_user = (1 << 6),

    vmem_flags_rw = (vmem_flags_read | vmem_flags_write),
} vmem_flags;

typedef enum {
    vmem_err_none = 0,
    vmem_err_alreadymapped = -1,
    vmem_err_continue = -2,
} vmem_errs;

int vmem_init();

int vmem_map(vmem_t *vm, intptr_t virt, intptr_t phys, size_t size, int perms, int flags);

int vmem_unmap(vmem_t *vm, intptr_t virt, size_t size);

int vmem_create(vmem_t *vm);

int vmem_setactive(vmem_t *vm);

int vmem_getactive(vmem_t **vm);

int vmem_flush(vmem_t *vm, intptr_t virt, int pg_cnt);

uint64_t vmem_virttophys(uint64_t virt);

intptr_t vmem_phystovirt(intptr_t phys, size_t sz);

#endif