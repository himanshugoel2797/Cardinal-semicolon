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

#include <cardinal/cs_error.h>
#include <stdint.h>
#include <stddef.h>

typedef struct vmem vmem_t;

typedef enum
{
    vmem_flags_read = 0,
    vmem_flags_write = (1 << 0),
    vmem_flags_exec = (1 << 1),
    vmem_flags_cachewritethrough = (1 << 2),
    vmem_flags_cachewriteback = (1 << 3),
    vmem_flags_cachewritecomplete = (1 << 4),
    vmem_flags_uncached = (1 << 5),

    vmem_flags_kernel = (1 << 10),
    vmem_flags_user = (1 << 11),

    vmem_flags_rw = (vmem_flags_read | vmem_flags_write),
} vmem_flags_t;

// Error codes are the unified cs_error set (see <cardinal/cs_error.h>).

cs_error vmem_init();

cs_error vmem_map(vmem_t *vm, intptr_t virt, intptr_t phys, size_t size, vmem_flags_t perms, vmem_flags_t flags);

cs_error vmem_unmap(vmem_t *vm, intptr_t virt, size_t size);

int vmem_create(vmem_t **vm);

void vmem_destroy(vmem_t *vm);

int vmem_setactive(vmem_t *vm);

int vmem_getactive(vmem_t **vm);

cs_error vmem_virttophys(vmem_t *vm, intptr_t virt, intptr_t *phys);

intptr_t vmem_phystovirt(intptr_t phys, size_t sz, vmem_flags_t flags);

intptr_t vmem_vmalloc(size_t sz);

void vmem_vfree(intptr_t virt, size_t sz);

// Set up the cross-core TLB-shootdown IPI. Call once after SysInterrupts/SysMP
// are up and before APs start scheduling (CALL:vmem_smp_init after CALL:mp_init).
cs_error vmem_smp_init();

// APIC id of the core a user address space is currently active on, or -1 if none.
// Snapshot this under the lock that performed the vmem_unmap, then pass it to
// vmem_shootdown after dropping locks (do not dereference the vmem_t while waiting).
int vmem_active_apic(vmem_t *vm);

// Complete a cross-core TLB invalidation for a range just unmapped or downgraded
// by vmem_unmap (which already flushed the local core). Kernel ranges (virt < 0)
// are broadcast to all other cores; user ranges are sent only to target_apic (from
// vmem_active_apic) when it is a different core. Must be called with interrupts
// enabled and holding no page-table or task lock.
void vmem_shootdown(intptr_t virt, size_t size, int target_apic);

#endif