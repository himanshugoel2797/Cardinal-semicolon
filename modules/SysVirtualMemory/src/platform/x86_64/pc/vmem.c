#include "SysVirtualMemory/vmem.h"
#include "SysPhysicalMemory/phys_mem.h"
#include "SysReg/registry.h"
#include "elf.h"
#include <cardinal/local_spinlock.h>
#include <stdint.h>
#include <string.h>
#include <stddef.h>
#include <stdlib.h>
#include <types.h>

#define PRESENT (1ull << 0)
#define WRITE (1ull << 1)
#define USER (1ull << 2)

#define WRITETHROUGH (1ull << 3)
#define CACHEDISABLE (1ull << 4)
#define WRITEBACK (0)
#define WRITECOMPLETE (3ull << 3)

#define LARGEPAGE (1ull << 7)
#define GLOBALPAGE (1ull << 8)
#define NOEXEC (1ull << 63)
#define ADDR_MASK (0x000ffffffffff000)

#define KERN_TOP_BASE (0xffffffff80000000)
#define KERN_PHYSMAP_BASE (0xFFFF800000000000)
#define KERN_PHYSMAP_BASE_UC (KERN_PHYSMAP_BASE + GiB(512))

struct vmem
{
    uint64_t ptable[256];
    int flags;
    int lock;
};

struct lcl_data
{
    uintptr_t ktable;
    vmem_t *cur_vmem;
};

static uint64_t levels[] = {
    GiB(512),
    GiB(1),
    MiB(2),
    KiB(4)};

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

static TLS struct lcl_data *lcl;
static vmem_t kmem;
static size_t phys_map_sz;

static uint64_t kernel_vmalloc = (KERN_PHYSMAP_BASE_UC + GiB(512));

//TODO: none of these operations are atomic to preemption within the kernel
intptr_t vmem_vmalloc(size_t sz)
{
    intptr_t rVal = (intptr_t)kernel_vmalloc;
    kernel_vmalloc += sz;
    return rVal;
}

void vmem_vfree(intptr_t virt, size_t sz)
{
    if (virt + sz == kernel_vmalloc)
        kernel_vmalloc -= sz;
}

int vmem_init()
{
    TLS void *(*mp_tls_get)(int) = (TLS void *(*)(int))elf_resolvefunction("mp_tls_get");
    int (*mp_tls_alloc)(int) = (int (*)(int))elf_resolvefunction("mp_tls_alloc");

    //Enable No Execute bit
    wrmsr(EFER_MSR, rdmsr(EFER_MSR) | (1 << 11));

    //Detect and enable SMEP/SMAP
    bool smep = false;
    registry_readkey_bool("HW/PROC", "SMEP", &smep);
    bool smap = false;
    registry_readkey_bool("HW/PROC", "SMAP", &smap);

    uint64_t cr4 = 0;
    __asm__ volatile("mov %%cr4, %0"
                     : "=r"(cr4)::);
    if (smep)
        cr4 |= (1 << 20);
    if (smap)
        cr4 |= (1 << 21);
    __asm__ volatile("mov %0, %%cr4" ::"r"(cr4));

    //Detect and enable 1GiB page support
    bool hugepage = false;
    registry_readkey_bool("HW/PROC", "HUGEPAGE", &hugepage);
    if (hugepage)
        largepage_avail[1] = true;

    //Setup PAT
    uint64_t pat = 0;
    pat |= 0x6;                   //PAT0 WB
    pat |= ((uint64_t)0x4) << 8;  //PAT1 WT
    pat |= ((uint64_t)0x0) << 16; //PAT2 UC
    pat |= ((uint64_t)0x1) << 24; //PAT3 WC
    wrmsr(PAT_MSR, pat);

    uintptr_t ktable_phys = pagealloc_alloc(-1, -1, physmem_alloc_flags_pagetable, KiB(4));
    uint64_t *ktable = (uint64_t *)vmem_phystovirt(ktable_phys, KiB(4), vmem_flags_cachewriteback);
    memset(ktable, 0, KiB(4));

    if (lcl == NULL)
        lcl = (TLS struct lcl_data *)mp_tls_get(mp_tls_alloc(sizeof(struct lcl_data)));
    lcl->ktable = ktable_phys;
    {
        kmem.flags = vmem_flags_kernel;
        kmem.lock = 0;
        memset(kmem.ptable, 0, sizeof(uint64_t) * 256);
    }

    lcl->cur_vmem = NULL;

    uint64_t cur_ptable = 0;
    __asm__ volatile("mov %%cr3, %0"
                     : "=r"(cur_ptable)::);
    uint64_t *cur_ptable_d = (uint64_t *)vmem_phystovirt(cur_ptable, KiB(4), vmem_flags_cachewriteback);

    vmem_map(NULL, KERN_TOP_BASE, 0x0, GiB(2), vmem_flags_kernel | vmem_flags_rw | vmem_flags_exec | vmem_flags_cachewriteback, 0);

    //Setup full physical to virtual map to simplify later accesses
    //registry_readkey_uint("HW/BOOTINFO", "MEMSIZE", &phys_map_sz);
    phys_map_sz = GiB(256);

    vmem_map(NULL, KERN_PHYSMAP_BASE, 0x0, phys_map_sz, vmem_flags_kernel | vmem_flags_rw | vmem_flags_cachewriteback, 0);
    vmem_map(NULL, KERN_PHYSMAP_BASE_UC, 0x0, phys_map_sz, vmem_flags_kernel | vmem_flags_rw | vmem_flags_uncached, 0);

    __asm__ volatile("mov %0, %%cr3" ::"r"(ktable_phys)
                     :);

    return 0;
}

int vmem_mp_init()
{
    TLS void *(*mp_tls_get)(int) = (TLS void *(*)(int))elf_resolvefunction("mp_tls_get");
    int (*mp_tls_alloc)(int) = (int (*)(int))elf_resolvefunction("mp_tls_alloc");

    //Enable No Execute bit
    wrmsr(EFER_MSR, rdmsr(EFER_MSR) | (1 << 11));

    //Detect and enable SMEP/SMAP
    bool smep = false;
    registry_readkey_bool("HW/PROC", "SMEP", &smep);
    bool smap = false;
    registry_readkey_bool("HW/PROC", "SMAP", &smap);

    uint64_t cr4 = 0;
    __asm__ volatile("mov %%cr4, %0"
                     : "=r"(cr4)::);
    if (smep)
        cr4 |= (1 << 20);
    if (smap)
        cr4 |= (1 << 21);
    __asm__ volatile("mov %0, %%cr4" ::"r"(cr4));

    //Detect and enable 1GiB page support
    bool hugepage = false;
    registry_readkey_bool("HW/PROC", "HUGEPAGE", &hugepage);
    if (hugepage)
        largepage_avail[1] = true;

    //Setup PAT
    uint64_t pat = 0;
    pat |= 0x6;                   //PAT0 WB
    pat |= ((uint64_t)0x4) << 8;  //PAT1 WT
    pat |= ((uint64_t)0x0) << 16; //PAT2 UC
    pat |= ((uint64_t)0x1) << 24; //PAT3 WC
    wrmsr(PAT_MSR, pat);

    uintptr_t ktable_phys = pagealloc_alloc(-1, -1, physmem_alloc_flags_pagetable, KiB(4));
    uint64_t *ktable = (uint64_t *)vmem_phystovirt(ktable_phys, KiB(4), vmem_flags_cachewriteback);
    memset(ktable, 0, KiB(4));

    lcl->ktable = ktable_phys;
    lcl->cur_vmem = NULL;

    local_spinlock_lock(&kmem.lock);
    memcpy(ktable + 256, kmem.ptable, sizeof(uint64_t) * 256);
    local_spinlock_unlock(&kmem.lock);

    __asm__ volatile("mov %0, %%cr3" ::"r"(ktable_phys)
                     :);

    return 0;
}

static int vmem_map_st(uint64_t *p_vm, uint64_t *vm, intptr_t virt, intptr_t phys, size_t size, int perms, int flags, int lv)
{
    uint64_t mask = masks[lv];
    uint64_t shamt = shamts[lv];
    uint64_t sz = levels[lv];

    uint64_t idx = (virt & mask) >> shamt;

    if (size % sz == 0 && largepage_avail[lv])
    {
        uint64_t c_flags = 0;
        c_flags |= PRESENT;

        if (perms & vmem_flags_write)
            c_flags |= WRITE;

        if (~perms & vmem_flags_exec)
            c_flags |= NOEXEC;

        if (perms & vmem_flags_cachewritethrough)
            c_flags |= WRITETHROUGH;
        else if (perms & vmem_flags_uncached)
            c_flags |= CACHEDISABLE;
        else if (perms & vmem_flags_cachewritecomplete)
            c_flags |= WRITECOMPLETE;
        else if (perms & vmem_flags_cachewriteback)
            c_flags |= WRITEBACK;

        if (perms & vmem_flags_user)
            c_flags |= USER;

        if (sz != KiB(4))
            c_flags |= LARGEPAGE;

        while (size > 0)
        {
            if (idx >= 512)
                return vmem_err_continue; //vmem_map_st(p_vm, p_vm, virt, phys, size, perms, flags, 0);

            if (vm[idx] & PRESENT)
                return vmem_err_alreadymapped;

            vm[idx] = (phys & ADDR_MASK) | c_flags;

            phys += sz;
            virt += sz;
            idx++;
            size -= sz;
        }
        return 0;
    }
    else
    {
        while (size > 0)
        {
            uint64_t n_lv = (vm[idx] & ADDR_MASK);

            if (n_lv == 0)
            {
                n_lv = pagealloc_alloc(-1, -1, physmem_alloc_flags_pagetable, KiB(4));
                if (n_lv == 0)
                    PANIC("Pagetable allocation failure!");

                memset((uint64_t *)vmem_phystovirt(n_lv, KiB(4), vmem_flags_cachewriteback), 0, KiB(4));
                vm[idx] = (n_lv & ADDR_MASK) | PRESENT | WRITE | USER;
            }

            if (vm[idx] & LARGEPAGE)
                return vmem_err_alreadymapped;

            uint64_t *n_lv_d = (uint64_t *)vmem_phystovirt(n_lv, KiB(4), vmem_flags_cachewriteback);

            int ret = vmem_map_st(p_vm, n_lv_d, virt, phys, size, perms, flags, lv + 1);
            if (ret != vmem_err_continue)
                return ret;

            uint64_t l_idx = (virt & masks[lv + 1]) >> shamts[lv + 1];
            uint64_t i_sz = (512 - l_idx) << shamts[lv + 1];

            size -= i_sz;
            virt += i_sz;
            phys += i_sz;

            idx++;
            if (idx >= 512)
                return vmem_err_continue;
        }

        return 0;
    }
}

static int vmem_unmap_st(uint64_t *p_vt, uint64_t *vm, intptr_t virt, size_t size, int lv)
{
    uint64_t mask = masks[lv];
    uint64_t shamt = shamts[lv];
    uint64_t sz = levels[lv];

    while (size > 0)
    {
        uint64_t idx = (virt & mask) >> shamt;
        uint64_t lv_ent = vm[idx];
        uint64_t *n_lv_d = (uint64_t *)vmem_phystovirt(lv_ent & ADDR_MASK, KiB(4), vmem_flags_cachewriteback);

        if (lv_ent & PRESENT)
        {
            if (size >= sz && ((lv_ent & LARGEPAGE) | (sz == KiB(4))))
            {
                vm[idx] = 0;
                size -= sz;
                virt += sz;
            }
            else if (size >= sz && (~lv_ent & LARGEPAGE))
            {
                //recurse lower
                int err = vmem_unmap_st(p_vt, n_lv_d, virt, size, lv + 1);
                if (err != 0)
                    return err;

                //free the lower level when done
                pagealloc_free(lv_ent & ADDR_MASK, KiB(4));
            }
            else if (size < sz && (~lv_ent & LARGEPAGE))
            {
                //Recurse lower
                return vmem_unmap_st(p_vt, n_lv_d, virt, size, lv + 1);
            }
            else if (size < sz && (lv_ent & LARGEPAGE))
            {
                //unmap large page
                //determine maximum size for unmapping desired portion
                //map at determined size
                PANIC("Unimplemented!");
            }
        }
        else if (size >= sz)
        {
            size -= sz;
            virt += sz;
        }
        else
            return vmem_err_nomapping;
    }

    return 0;
}

int vmem_map(vmem_t *vm, intptr_t virt, intptr_t phys, size_t size, int perms, int flags)
{
    uint64_t *ptable = 0;

    if (virt < 0)
    {
        //Add to kernel map
        local_spinlock_lock(&kmem.lock);
        ptable = (uint64_t *)kmem.ptable - 256;
    }
    else
    {
        //Add to user map
        local_spinlock_lock(&vm->lock);
        ptable = vm->ptable;
    }

    int rVal = vmem_map_st(ptable, ptable, virt, phys, size, perms, flags, 0);
    if (virt >= 0)
        local_spinlock_unlock(&vm->lock);
    else
    {
        uint64_t *p_table = (uint64_t *)vmem_phystovirt(lcl->ktable, KiB(4), vmem_flags_cachewriteback);
        memcpy(p_table + 256, kmem.ptable, 256 * sizeof(uint64_t));
        local_spinlock_unlock(&kmem.lock);
    }
    return rVal;
}

int vmem_unmap(vmem_t *vm, intptr_t virt, size_t size)
{
    uint64_t *ptable = 0;
    size = 0;

    if (virt < 0)
    {
        //Add to kernel map
        local_spinlock_lock(&kmem.lock);
        ptable = (uint64_t *)kmem.ptable - 256;
    }
    else
    {
        //Add to user map
        local_spinlock_lock(&vm->lock);
        ptable = vm->ptable;
    }

    int rVal = vmem_unmap_st(ptable, ptable, virt, size, 0);
    if (virt >= 0)
        local_spinlock_unlock(&vm->lock);
    else
    {
        uint64_t *p_table = (uint64_t *)vmem_phystovirt(lcl->ktable, KiB(4), vmem_flags_cachewriteback);
        memcpy(p_table + 256, kmem.ptable, 256 * sizeof(uint64_t));
        local_spinlock_unlock(&kmem.lock);
    }

    return rVal;
}

int vmem_create(vmem_t **vm_r)
{
    vmem_t *vm = malloc(sizeof(vmem_t));
    if (vm == NULL)
        return -1;

    vm->flags = vmem_flags_user;
    vm->lock = 0;
    memset(vm->ptable, 0, 256 * sizeof(uint64_t));
    *vm_r = vm;

    return 0;
}

void vmem_destroy(vmem_t *vm_r)
{
    if (vm_r != NULL)
    {
        local_spinlock_lock(&vm_r->lock);
        free(vm_r);
    }
}

static void vmem_savestate()
{
    if (lcl->cur_vmem != NULL)
    {
        vmem_t *vmem = lcl->cur_vmem;
        local_spinlock_lock(&vmem->lock);
        //copy state from ktable
        uint64_t *p_table = (uint64_t *)vmem_phystovirt(lcl->ktable, KiB(4), vmem_flags_cachewriteback);
        memcpy(vmem->ptable, p_table, 256 * sizeof(uint64_t));
        local_spinlock_unlock(&vmem->lock);
    }

    {
        local_spinlock_lock(&kmem.lock);
        //copy state from ktable
        uint64_t *p_table = (uint64_t *)vmem_phystovirt(lcl->ktable, KiB(4), vmem_flags_cachewriteback);
        memcpy(kmem.ptable, p_table + 256, 256 * sizeof(uint64_t));
        local_spinlock_unlock(&kmem.lock);
    }
}

int vmem_setactive(vmem_t *vm)
{
    vmem_savestate();

    //copy state to ktable
    local_spinlock_lock(&vm->lock);
    uint64_t *p_table = (uint64_t *)vmem_phystovirt(lcl->ktable, KiB(4), vmem_flags_cachewriteback);
    memcpy(p_table, vm->ptable, 256 * sizeof(uint64_t));
    local_spinlock_unlock(&vm->lock);

    lcl->cur_vmem = vm;
    __asm__ volatile("mov %0, %%cr3" ::"r"(lcl->ktable)
                     :);

    return 0;
}

int vmem_getactive(vmem_t **vm)
{
    vmem_savestate();
    *vm = lcl->cur_vmem;
    return 0;
}

int vmem_flush(intptr_t virt, size_t sz)
{
    //TODO: shootdown cores when kmem flushed
    // Alternatively, every 'thread' has its own memory space, so changes don't affect other cores
    //Thus, kernel can be flushed once every task switch

    vmem_savestate();

    if (sz > GiB(1))
        __asm__ volatile("mov %0, %%cr3" ::"r"(lcl->ktable)
                         :);
    else
    {
        for (size_t n = 0; n < sz; n += KiB(4), virt += KiB(4))
            __asm__("invlpg (%0)" ::"r"(virt)
                    :);
    }
    return 0;
}

static int vmem_virttophys_st(uint64_t *pg, uint64_t virt, intptr_t *phys, int lv)
{
    uint64_t shamt = shamts[lv];
    uint64_t mask = masks[lv];

    uint64_t idx = (virt & mask) >> shamt;
    uint64_t ent = pg[idx];

    if (ent & PRESENT)
    {

        if ((ent & LARGEPAGE) && largepage_avail[lv])
        {
            *phys = (ent & ADDR_MASK) + (virt % levels[lv]);
            return 0;
        }

        if (levels[lv] == KiB(4))
        {
            *phys = (ent & ADDR_MASK) + (virt % levels[lv]);
            return 0;
        }

        uint64_t *n_lv_d = (uint64_t *)vmem_phystovirt(ent & ADDR_MASK, KiB(4), vmem_flags_cachewriteback);
        return vmem_virttophys_st(n_lv_d, virt, phys, lv + 1);
    }
    else
        return -1;
}

int vmem_virttophys(intptr_t virt, intptr_t *phys)
{
    uint64_t *n_lv_d = (uint64_t *)vmem_phystovirt(lcl->ktable, KiB(4), vmem_flags_cachewriteback);
    return vmem_virttophys_st(n_lv_d, (uint64_t)virt, phys, 0);
}

intptr_t vmem_phystovirt(intptr_t phys, size_t sz, int flags)
{

    if (flags & vmem_flags_cachewriteback)
    {
        if (phys < (intptr_t)GiB(2) && (phys + sz) < (intptr_t)GiB(2))
            return (phys + KERN_TOP_BASE);

        if (phys < (intptr_t)phys_map_sz && (phys + sz) < phys_map_sz)
            return (phys + KERN_PHYSMAP_BASE);
    }
    else if (flags & vmem_flags_uncached)
    {
        if (phys < (intptr_t)phys_map_sz && (phys + sz) < phys_map_sz)
            return (phys + KERN_PHYSMAP_BASE_UC);
    }

    char tmp[20];
    DEBUG_PRINT(ltoa(phys, tmp, 16));
    PANIC("Invalid Address Detected!");
    return phys;
}