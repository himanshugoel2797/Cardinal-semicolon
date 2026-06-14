#include "SysVirtualMemory/vmem.h"
#include "SysPhysicalMemory/phys_mem.h"
#include "SysReg/registry.h"
#include "SysInterrupts/interrupts.h" //IPI enums/typedefs only; functions resolved at runtime
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
static int kernel_vmalloc_lock = 0;

intptr_t vmem_vmalloc(size_t sz)
{
    local_spinlock_lock(&kernel_vmalloc_lock);
    intptr_t rVal = (intptr_t)kernel_vmalloc;
    kernel_vmalloc += sz;
    local_spinlock_unlock(&kernel_vmalloc_lock);
    return rVal;
}

void vmem_vfree(intptr_t virt, size_t sz)
{
    local_spinlock_lock(&kernel_vmalloc_lock);
    //Only the most recent allocation can be returned to this bump allocator
    if (virt + sz == kernel_vmalloc)
        kernel_vmalloc -= sz;
    local_spinlock_unlock(&kernel_vmalloc_lock);
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
    if (ktable_phys == PHYSMEM_NO_ALLOC)
        PANIC("Failed to allocate kernel pagetable!");
    uint64_t *ktable = (uint64_t *)vmem_phystovirt(ktable_phys, KiB(4), vmem_flags_cachewriteback);
    memset(ktable, 0, KiB(4));

    if (lcl == NULL)
    {
        //mp_tls_alloc(8);
        lcl = (TLS struct lcl_data *)mp_tls_get(mp_tls_alloc(sizeof(struct lcl_data)));
    }
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

    //Pre-create the top-level (PML4) entry covering the kernel vmalloc region.
    //Each AP snapshots the kernel half of the page table once, in vmem_mp_init;
    //a kernel mapping that creates a *new* PML4 entry after that snapshot is
    //invisible to that AP (the kernel half is never refreshed on a running core).
    //By establishing the vmalloc PML4 entry here, before any AP boots, every core
    //inherits it, and later vmalloc-backed kernel mappings land under the shared
    //PDPT and stay coherent across cores.
    {
        intptr_t pre = vmem_vmalloc(KiB(4));
        uintptr_t prep = pagealloc_alloc(-1, -1, physmem_alloc_flags_data, KiB(4));
        if (prep == PHYSMEM_NO_ALLOC)
            PANIC("Failed to reserve kernel vmalloc region!");
        vmem_map(NULL, pre, (intptr_t)prep, KiB(4), vmem_flags_kernel | vmem_flags_rw, 0);
    }

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
    if (ktable_phys == PHYSMEM_NO_ALLOC)
        PANIC("Failed to allocate kernel pagetable!");
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

        if ((perms & vmem_flags_exec) == 0)
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
                if (n_lv == PHYSMEM_NO_ALLOC)
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

    //kmem.lock / vm->lock are also taken by the preemption-timer ISR (via
    //vmem_setactive). Hold interrupts off so this core cannot be preempted while
    //holding them, which would otherwise self-deadlock (or ABBA across cores).
    int cli_state = cli();
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
    sti(cli_state);
    return rVal;
}

int vmem_unmap(vmem_t *vm, intptr_t virt, size_t size)
{
    uint64_t *ptable = 0;

    //See vmem_map: interrupts off while holding kmem.lock / vm->lock.
    int cli_state = cli();
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
    sti(cli_state);

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
        int cli_state = cli();
        local_spinlock_lock(&vm_r->lock);
        free(vm_r);
        sti(cli_state);
    }
}

static void vmem_savestate()
{
    //Runs both standalone (task context) and from the preemption-timer ISR via
    //vmem_setactive. cli()/sti() nest correctly: when already in the ISR (IF=0)
    //cli() returns 0 and sti(0) leaves interrupts off.
    int cli_state = cli();
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
    sti(cli_state);
}

int vmem_setactive(vmem_t *vm)
{
    vmem_savestate();

    //copy state to ktable
    int cli_state = cli();
    local_spinlock_lock(&vm->lock);
    uint64_t *p_table = (uint64_t *)vmem_phystovirt(lcl->ktable, KiB(4), vmem_flags_cachewriteback);
    memcpy(p_table, vm->ptable, 256 * sizeof(uint64_t));
    local_spinlock_unlock(&vm->lock);

    lcl->cur_vmem = vm;
    __asm__ volatile("mov %0, %%cr3" ::"r"(lcl->ktable)
                     :);
    sti(cli_state);

    return 0;
}

int vmem_getactive(vmem_t **vm)
{
    vmem_savestate();
    *vm = lcl->cur_vmem;
    return 0;
}

// --- Cross-core TLB shootdown (SMP) ---------------------------------------
//
// Invalidating a *kernel* (shared, virt < 0) mapping has to reach every core: a
// stale TLB entry on another core could keep reading/writing a page after it is
// unmapped and its physical frame reused. vmem_flush therefore broadcasts a
// shootdown IPI to the other cores and waits for each to acknowledge before
// returning, so the caller knows the invalidation is globally complete before it
// frees/reuses the frame.
//
// Scope/limitations (see notes/AUDIT.md):
//  - User mappings (virt >= 0) live in per-task page tables that are only ever
//    active on one core at a time, and a task switch reloads cr3 (flushing
//    non-global entries), so a *local* invalidate is sufficient -- no IPI.
//  - The kernel half of the page table is still synced between cores coarsely,
//    by memcpy of kmem.ptable in vmem_savestate/setactive. The shootdown handler
//    refreshes the receiving core's kernel half from kmem.ptable before flushing,
//    so a newly *created* kernel mapping also becomes visible there. Fully
//    race-free concurrent kernel-map mutation additionally wants a kernel PML4
//    that is physically shared across cores (so savestate cannot write a stale
//    half back to the master); that rework is tracked separately.
//  - Contract: call vmem_flush with interrupts enabled and without holding
//    kmem.lock -- the handler takes kmem.lock, and the initiator must remain
//    able to service an inbound shootdown while it waits.
#define VMEM_MAX_CORES 256
static int tlb_vec = -1;                 // IPI vector, -1 until vmem_smp_init
static int tlb_lock = 0;                 // serialises shootdown initiators
static volatile intptr_t tlb_virt;       // range being shot down (under tlb_lock)
static volatile size_t tlb_size;
static volatile int tlb_ack;             // set by the target after it flushes
static int tlb_apicids[VMEM_MAX_CORES];  // APIC ids of the *other* cores
static int tlb_other_count = 0;
static void (*tlb_sendipi)(int, int, ipi_delivery_mode_t) = NULL;

static void tlb_local_flush(intptr_t virt, size_t sz)
{
    if (sz > GiB(1))
        __asm__ volatile("mov %0, %%cr3" ::"r"(lcl->ktable)
                         :);
    else
        for (size_t n = 0; n < sz; n += KiB(4), virt += KiB(4))
            __asm__ volatile("invlpg (%0)" ::"r"(virt)
                             : "memory");
}

//IPI handler (runs in interrupt context on a *receiving* core). Refresh this
//core's kernel-half page-table copy from the master, then flush, then ack.
static void tlb_shootdown_handler(int irq)
{
    (void)irq;
    intptr_t v = tlb_virt;
    size_t s = tlb_size;

    local_spinlock_lock(&kmem.lock);
    uint64_t *p_table = (uint64_t *)vmem_phystovirt(lcl->ktable, KiB(4), vmem_flags_cachewriteback);
    memcpy(p_table + 256, kmem.ptable, 256 * sizeof(uint64_t));
    local_spinlock_unlock(&kmem.lock);

    tlb_local_flush(v, s);
    __atomic_store_n(&tlb_ack, 1, __ATOMIC_SEQ_CST);
}

int vmem_flush(intptr_t virt, size_t sz)
{
    vmem_savestate();
    tlb_local_flush(virt, sz);

    //Only kernel (shared) mappings need a cross-core shootdown, and only once
    //other cores are online and the IPI vector has been set up.
    if (virt < 0 && tlb_vec >= 0 && tlb_other_count > 0)
    {
        local_spinlock_lock(&tlb_lock);
        tlb_virt = virt;
        tlb_size = sz;
        for (int i = 0; i < tlb_other_count; i++)
        {
            __atomic_store_n(&tlb_ack, 0, __ATOMIC_SEQ_CST);
            tlb_sendipi(tlb_apicids[i], tlb_vec, ipi_delivery_mode_fixed);
            //One core at a time: also avoids overrunning the local APIC ICR.
            while (__atomic_load_n(&tlb_ack, __ATOMIC_SEQ_CST) == 0)
                __asm__ volatile("pause");
        }
        local_spinlock_unlock(&tlb_lock);
    }
    return 0;
}

//Set up the cross-core TLB-shootdown IPI. Must run after SysInterrupts and the
//AP enumeration are up (acpi_init populated HW/LAPIC, intr_init armed the APIC)
//but before APs start scheduling -- i.e. CALL:vmem_smp_init after CALL:mp_init in
//loadscript.txt. SysInterrupts/SysMP load after SysVirtualMemory, so their
//entry points are resolved at runtime via the kernel symbol DB, not linked.
int vmem_smp_init()
{
    int (*intr_allocate)(int, interrupt_flags_t, int *) =
        (int (*)(int, interrupt_flags_t, int *))elf_resolvefunction("interrupt_allocate");
    void (*intr_register)(int, InterruptHandler) =
        (void (*)(int, InterruptHandler))elf_resolvefunction("interrupt_registerhandler");
    int (*intr_cpuidx)(void) = (int (*)(void))elf_resolvefunction("interrupt_get_cpuidx");
    tlb_sendipi = (void (*)(int, int, ipi_delivery_mode_t))elf_resolvefunction("interrupt_sendipi");

    if (intr_allocate == NULL || intr_register == NULL || intr_cpuidx == NULL || tlb_sendipi == NULL)
        return -1;

    //Enumerate every core's APIC id from the registry, recording all but our own.
    int self = intr_cpuidx();
    uint64_t count = 0;
    registry_readkey_uint("HW/LAPIC", "COUNT", &count);
    tlb_other_count = 0;
    for (uint64_t i = 0; i < count && tlb_other_count < VMEM_MAX_CORES; i++)
    {
        char idx_str[16] = "";
        char key_str[256] = "HW/LAPIC/";
        char *key = strncat(key_str, itoa((int)i, idx_str, 16), 255);

        uint64_t apic_id = 0;
        if (registry_readkey_uint(key, "APIC ID", &apic_id) != registry_err_ok)
            continue;
        if ((int)apic_id == self)
            continue;
        tlb_apicids[tlb_other_count++] = (int)apic_id;
    }

    //Allocate a dedicated vector and register the handler once (the handler table
    //is global, so this one registration covers every core's IDT).
    int base = 0;
    if (intr_allocate(1, interrupt_flags_exclusive, &base) != 0)
        return -1;
    intr_register(base, tlb_shootdown_handler);
    tlb_vec = base;
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

int vmem_virttophys(vmem_t *vm, intptr_t virt, intptr_t *phys)
{
    if (virt < 0)
    {
        //kernel address
        uint64_t *n_lv_d = (uint64_t *)vmem_phystovirt(lcl->ktable, KiB(4), vmem_flags_cachewriteback);
        return vmem_virttophys_st(n_lv_d, (uint64_t)virt, phys, 0);
    }
    else if (vm != NULL)
    {
        //user address
        uint64_t *n_lv_d = vm->ptable;
        return vmem_virttophys_st(n_lv_d, (uint64_t)virt, phys, 0);
    }
    return -2;
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