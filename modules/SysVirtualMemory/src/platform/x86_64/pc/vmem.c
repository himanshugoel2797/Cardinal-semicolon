#include "SysVirtualMemory/vmem.h"
#include "SysPhysicalMemory/phys_mem.h"
#include "SysMP/mp.h"
#include <stdint.h>
#include <string.h>
#include <stddef.h>
#include <types.h>

#define PRESENT (1ull << 0)
#define WRITE (1ull << 1)
#define USER (1ull << 2)
#define WRITETHROUGH (1ull << 3)
#define CACHEDISABLE (1ull << 4)
#define LARGEPAGE (1ull << 7)
#define GLOBALPAGE (1ull << 8)
#define NOEXEC (1ull << 63)
#define ADDR_MASK (0x000ffffffffff000)

struct vmem {
    uint64_t *ptable;
};

static uint64_t levels[] = {
    GiB(512),
    GiB(1),
    MiB(2),
    KiB(4)
};

static uint64_t masks[] = {
    0xff8000000000,
    0x007fC0000000,
    0x00003fE00000,
    0x0000001ff000,
};

static uint64_t shamts[] = {
    39,
    30,
    21,
    12,
};

static bool largepage_avail[] = {
    false,
    false,
    true,
    true,
};

static uint64_t *ktable;

int vmem_init(){

    //Virt: 0xffffffff800000000 Phys: 0x0 Size: GiB(1) RWX

    uintptr_t ktable_phys = pagealloc_alloc(-1, -1, physmem_alloc_flags_pagetable, KiB(4));
    ktable = (uint64_t*)vmem_phystovirt(ktable_phys, KiB(4));
    memset(ktable, 0, KiB(4));

    uint64_t cur_ptable = 0;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cur_ptable) ::);
    uint64_t *cur_ptable_d = (uint64_t*)vmem_phystovirt(cur_ptable, KiB(4));

    //vmem_map(NULL, 0, 0, MiB(2), vmem_flags_kernel | vmem_flags_rw | vmem_flags_exec | vmem_flags_cachewriteback, 0);
    vmem_map(NULL, 0xffffffff80000000, 0x0, GiB(2), vmem_flags_kernel | vmem_flags_rw | vmem_flags_exec | vmem_flags_cachewriteback, 0);
    
    __asm__ volatile("mov %0, %%cr3\r\nhlt" :: "r"(ktable_phys) :);

    return 0;
}

static int vmem_map_st(uint64_t *p_vm, uint64_t *vm, intptr_t virt, intptr_t phys, size_t size, int perms, int flags, int lv) {
    uint64_t mask = masks[lv];
    uint64_t shamt = shamts[lv];
    uint64_t sz = levels[lv];

    uint64_t idx = (virt & mask) >> shamt;

    if(size % sz == 0 && largepage_avail[lv]) {
        uint64_t c_flags = 0;
        c_flags |= PRESENT;
        
        if(perms & vmem_flags_write)
            c_flags |= WRITE;

        if(~perms & vmem_flags_exec)
            c_flags |= NOEXEC;
        
        if(perms & vmem_flags_cachewritethrough)
            c_flags |= WRITETHROUGH;

        if(perms & vmem_flags_uncached)
            c_flags |= CACHEDISABLE;

        if(perms & vmem_flags_user)
            c_flags |= USER;

        if (sz != KiB(4))
            c_flags |= LARGEPAGE;
        
        while(size > 0) {
            if(idx >= 512)
                return vmem_err_continue;//vmem_map_st(p_vm, p_vm, virt, phys, size, perms, flags, 0);

            if(vm[idx] & PRESENT)
                return vmem_err_alreadymapped;

            vm[idx] = (phys & ADDR_MASK) | c_flags;

            phys += sz;
            virt += sz;
            idx++;
            size -= sz;
        }
        return 0;
    }else{
        while(size > 0) {
            uint64_t n_lv = (vm[idx] & ADDR_MASK);

            if(n_lv == 0) {
                n_lv = pagealloc_alloc(-1, -1, physmem_alloc_flags_pagetable, KiB(4));
                if(n_lv == 0)
                    PANIC("Pagetable allocation failure!");

                memset((uint64_t*)vmem_phystovirt(n_lv, KiB(4)), 0, KiB(4));
                vm[idx] = (n_lv & ADDR_MASK) | PRESENT | WRITE | USER;
            }

            if(vm[idx] & LARGEPAGE)
                return vmem_err_alreadymapped;

            uint64_t *n_lv_d = (uint64_t*)vmem_phystovirt(n_lv, KiB(4));

            int ret = vmem_map_st(p_vm, n_lv_d, virt, phys, size, perms, flags, lv + 1);
            if(ret != vmem_err_continue)
                return ret;

            uint64_t l_idx = (virt & masks[lv + 1]) >> shamts[lv + 1];
            uint64_t i_sz = (512 - l_idx) << shamts[lv + 1];

            size -= i_sz;
            virt += i_sz;
            phys += i_sz;

            idx++;
            if(idx >= 512)
                return vmem_err_continue;
        }

        return 0;
    }
}

int vmem_map(vmem_t *vm, intptr_t virt, intptr_t phys, size_t size, int perms, int flags) {
    uint64_t *ptable = 0;

    if(virt < 0) {
        //Add to kernel map
        ptable = ktable;
    } else {
        //Add to user map
        ptable = vm->ptable;
    }

    return vmem_map_st(ptable, ptable, virt, phys, size, perms, flags, 0);
}

int vmem_unmap(vmem_t *vm, intptr_t virt, size_t size) {
    uint64_t *ptable = 0;
    size = 0;

    if(virt < 0) {
        //Add to kernel map
        ptable = ktable;
    } else {
        //Add to user map
        ptable = vm->ptable;
    }

    return 0;
}

int vmem_create(vmem_t *vm) {
    vm = NULL;
    PANIC("Unimplemented!");
    return 0;
}

int vmem_setactive(vmem_t *vm) {
    vm = NULL;
    PANIC("Unimplemented!");
    return 0;
}

int vmem_getactive(vmem_t **vm) {
    vm = NULL;
    PANIC("Unimplemented!");
    return 0;
}

int vmem_flush(vmem_t *vm, intptr_t virt, int pg_cnt) {
    vm = NULL;
    virt = 0;
    pg_cnt = 0;
    PANIC("Unimplemented!");
    return 0;
}

uint64_t vmem_virttophys(uint64_t virt) {
    virt = 0;
    PANIC("Unimplemented!");
    return 0;
}

intptr_t vmem_phystovirt(intptr_t phys, size_t sz){
    if(phys < (intptr_t)GiB(2) && (phys + sz) < (intptr_t)GiB(2))
        return (phys + 0xffffffff80000000);

    PANIC("Unimplemented!");
    return phys;
}