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
#define KERN_PHYSMAP_BASE_WC (KERN_PHYSMAP_BASE_UC + GiB(512))

struct vmem
{
    uintptr_t pml4_phys; //physical address of this address space's PML4 page
    uint64_t *pml4;      //physmap pointer to that 512-entry PML4 page
    int flags;
    int lock;
    volatile int active_apic; //APIC id of the core this AS is active on, -1 if none.
                              //One task per AS and no migration, so at most one core.
};

struct lcl_data
{
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

static uint64_t kernel_vmalloc = (KERN_PHYSMAP_BASE_WC + GiB(512));
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

// --- Cross-core TLB shootdown (SMP) --------------------------------------
//
// vmem_unmap / a permission downgrade edits the page tables but only the editing
// core's own TLB is flushed locally (vmem_local_flush). Any *other* core that has
// the same translation cached must be told to invalidate it before the physical
// frame is freed/reused or the (now stricter) permissions are relied upon:
//   - kernel ranges (virt < 0) are shared in every address space -> broadcast to
//     every other core;
//   - user ranges live in a per-task address space that is active on at most one
//     core (one task per AS, no migration/threads) -> shoot down just that core,
//     identified by vmem_t.active_apic snapshotted at unmap time.
// A core that *enters* an address space does a cr3 load (a full flush, since
// kernel pages are not global), so it never needs a shootdown for an edit that
// was already visible in the shared page tables before it loaded cr3.
//
// Contract: call vmem_shootdown with interrupts ENABLED and holding no page-table
// or task lock -- it busy-waits for each target to ack from interrupt context, so
// a target blocked with interrupts off on a lock the caller holds would deadlock.
#define VMEM_MAX_CORES 256
static int tlb_vec = -1;                   //shootdown IPI vector, -1 until vmem_smp_init
static int tlb_lock = 0;                   //serialises shootdown initiators
static volatile intptr_t tlb_virt;         //range under shootdown (guarded by tlb_lock)
static volatile size_t tlb_size;
static volatile int tlb_ack;               //set by the target once it has flushed
static int tlb_all_apics[VMEM_MAX_CORES];  //every core's APIC id (from the MADT)
static int tlb_core_count = 0;
static void (*tlb_sendipi)(int, int, ipi_delivery_mode_t) = NULL;
static int (*tlb_self_apic)(void) = NULL;  //interrupt_get_cpu_idx (returns this core's APIC id)

//Invalidate [virt, virt+sz) in THIS core's TLB for the currently-active address
//space. Above a threshold a cr3 reload (which also flushes the kernel half, as
//kernel pages are not global) is cheaper than a long invlpg sweep.
static void vmem_local_flush(intptr_t virt, size_t sz)
{
    if (sz >= GiB(1))
    {
        uintptr_t cr3;
        __asm__ volatile("mov %%cr3, %0"
                         : "=r"(cr3)::);
        __asm__ volatile("mov %0, %%cr3" ::"r"(cr3)
                         : "memory");
    }
    else
    {
        for (size_t n = 0; n < sz; n += KiB(4), virt += KiB(4))
            __asm__ volatile("invlpg (%0)" ::"r"(virt)
                             : "memory");
    }
}

//Per-core CPU/MMU feature setup that must run on every core (the BSP in
//vmem_init and each AP in vmem_mp_init): enable NX, SMEP/SMAP and 1GiB pages,
//and program the PAT. These are per-core MSRs and control registers.
static void vmem_percore_arch_init(void)
{
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
}

cs_error vmem_init()
{
    TLS void *(*mp_tls_get)(int) = (TLS void *(*)(int))elf_resolvefunction("mp_tls_get");
    int (*mp_tls_alloc)(int) = (int (*)(int))elf_resolvefunction("mp_tls_alloc");

    vmem_percore_arch_init();

    //The master kernel PML4. Its upper 256 entries (the kernel half) ARE the
    //shared kernel address space: every process's PML4 copies them once at
    //creation, and because they point at shared lower-level tables, later
    //kernel-map changes are visible in every address space with no per-core
    //resync. A core's cr3 points here whenever it is not running a task.
    uintptr_t kpml4_phys = physmem_alloc(-1, -1, physmem_alloc_flags_pagetable, KiB(4));
    if (kpml4_phys == PHYSMEM_NO_ALLOC)
        PANIC("Failed to allocate kernel pagetable!");
    kmem.pml4_phys = kpml4_phys;
    kmem.pml4 = (uint64_t *)vmem_phystovirt(kpml4_phys, KiB(4), vmem_flags_cachewriteback);
    memset(kmem.pml4, 0, KiB(4));
    kmem.flags = vmem_flags_kernel;
    kmem.lock = 0;

    if (lcl == NULL)
        lcl = (TLS struct lcl_data *)mp_tls_get(mp_tls_alloc(sizeof(struct lcl_data)));
    lcl->cur_vmem = NULL;

    vmem_map(NULL, KERN_TOP_BASE, 0x0, GiB(2), vmem_flags_kernel | vmem_flags_rw | vmem_flags_exec | vmem_flags_cachewriteback, 0);

    //Setup full physical to virtual map to simplify later accesses
    //registry_readkey_uint("HW/BOOTINFO", "MEMSIZE", &phys_map_sz);
    phys_map_sz = GiB(256);

    vmem_map(NULL, KERN_PHYSMAP_BASE, 0x0, phys_map_sz, vmem_flags_kernel | vmem_flags_rw | vmem_flags_cachewriteback, 0);
    vmem_map(NULL, KERN_PHYSMAP_BASE_UC, 0x0, phys_map_sz, vmem_flags_kernel | vmem_flags_rw | vmem_flags_uncached, 0);
    // Write-combining window: same identity layout, PAT3 (WC) page bits. Lets a
    // framebuffer be mapped WC so the CPU coalesces sequential stores into burst
    // writes -- ~10-50x the throughput of the UC window for streaming pixel data.
    vmem_map(NULL, KERN_PHYSMAP_BASE_WC, 0x0, phys_map_sz, vmem_flags_kernel | vmem_flags_rw | vmem_flags_cachewritecomplete, 0);

    //Pre-create the top-level (PML4) entry covering the kernel vmalloc region.
    //The shared-kernel-PML4 model requires every kernel PML4 *entry* to exist
    //before any address space is created: a process's PML4 copies the kernel
    //entries once at creation, so a kernel mapping that creates a *new* PML4 entry
    //afterwards would be invisible to already-created address spaces. Lower-level
    //tables are shared, so anything under an existing PML4 entry stays coherent;
    //only brand-new PML4 entries are a problem. The physmap and kernel-top entries
    //above already cover their ranges; this reserves the vmalloc PML4 entry too,
    //so all runtime kernel mappings land under an already-shared PDPT.
    {
        intptr_t pre = vmem_vmalloc(KiB(4));
        uintptr_t prep = physmem_alloc(-1, -1, physmem_alloc_flags_data, KiB(4));
        if (prep == PHYSMEM_NO_ALLOC)
            PANIC("Failed to reserve kernel vmalloc region!");
        vmem_map(NULL, pre, (intptr_t)prep, KiB(4), vmem_flags_kernel | vmem_flags_rw, 0);
    }

    //Reserve every kernel-half PML4 entry (256..511) up front so the
    //shared-kernel-PML4 model stays correct however kernel mappings grow later.
    //A process PML4 copies these 256 entries once at creation (vmem_create);
    //a kernel mapping added afterwards under an *existing* entry propagates via
    //the shared lower-level tables, but a brand-new top-level entry created
    //after an address space was cloned would be invisible to it. The maps above
    //only populate entries 256/257/258 (physmap WB/UC/WC), 259 (vmalloc) and 511
    //(kernel image); installing an empty (zeroed) PDPT for the remaining
    //kernel-half entries makes all 256 present and shared before any AP boots or
    //any task is created, so vmalloc (or any future kernel region) can grow
    //across a 512 GiB PML4 boundary with no per-address-space resync. Cost is at
    //most 256 PDPT pages (1 MiB) pinned for the life of the system. The empty
    //PDPTs map nothing, so accesses under them still fault until explicitly
    //mapped. Pre-AP, pre-scheduler init; the lock mirrors vmem_map's discipline.
    {
        int cli_state = cli();
        local_spinlock_lock(&kmem.lock);
        for (int i = 256; i < 512; i++)
        {
            if (kmem.pml4[i] & PRESENT)
                continue;
            uintptr_t pdpt = physmem_alloc(-1, -1, physmem_alloc_flags_pagetable, KiB(4));
            if (pdpt == PHYSMEM_NO_ALLOC)
                PANIC("Failed to reserve kernel PML4 entry!");
            memset((uint64_t *)vmem_phystovirt(pdpt, KiB(4), vmem_flags_cachewriteback), 0, KiB(4));
            kmem.pml4[i] = (pdpt & ADDR_MASK) | PRESENT | WRITE | USER;
        }
        local_spinlock_unlock(&kmem.lock);
        sti(cli_state);
    }

    __asm__ volatile("mov %0, %%cr3" ::"r"(kpml4_phys)
                     :);

    return 0;
}

cs_error vmem_mp_init()
{
    vmem_percore_arch_init();

    //No per-core kernel page table any more: every core runs on the shared kernel
    //PML4 (kmem) until it schedules a task with its own address space, at which
    //point vmem_setactive points cr3 at that task's PML4 (whose kernel half is a
    //copy of kmem's). The kernel half is identical everywhere, so there is nothing
    //per-core to snapshot here.
    lcl->cur_vmem = NULL;
    __asm__ volatile("mov %0, %%cr3" ::"r"(kmem.pml4_phys)
                     :);

    return 0;
}

static int vmem_map_st(uint64_t *p_vm, uint64_t *vm, intptr_t virt, intptr_t phys, size_t size, vmem_flags_t perms, vmem_flags_t flags, int lv)
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
                return CS_CONTINUE; //vmem_map_st(p_vm, p_vm, virt, phys, size, perms, flags, 0);

            if (vm[idx] & PRESENT)
                return CS_ALREADYMAPPED;

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
                n_lv = physmem_alloc(-1, -1, physmem_alloc_flags_pagetable, KiB(4));
                if (n_lv == PHYSMEM_NO_ALLOC)
                    PANIC("Pagetable allocation failure!");

                memset((uint64_t *)vmem_phystovirt(n_lv, KiB(4), vmem_flags_cachewriteback), 0, KiB(4));
                vm[idx] = (n_lv & ADDR_MASK) | PRESENT | WRITE | USER;
            }

            if (vm[idx] & LARGEPAGE)
                return CS_ALREADYMAPPED;

            uint64_t *n_lv_d = (uint64_t *)vmem_phystovirt(n_lv, KiB(4), vmem_flags_cachewriteback);

            int ret = vmem_map_st(p_vm, n_lv_d, virt, phys, size, perms, flags, lv + 1);
            if (ret != CS_CONTINUE)
                return ret;

            uint64_t l_idx = (virt & masks[lv + 1]) >> shamts[lv + 1];
            uint64_t i_sz = (512 - l_idx) << shamts[lv + 1];

            size -= i_sz;
            virt += i_sz;
            phys += i_sz;

            idx++;
            if (idx >= 512)
                return CS_CONTINUE;
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
                physmem_free(lv_ent & ADDR_MASK, KiB(4));
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
            return CS_NOMAPPING;
    }

    return 0;
}

cs_error vmem_map(vmem_t *vm, intptr_t virt, intptr_t phys, size_t size, vmem_flags_t perms, vmem_flags_t flags)
{
    uint64_t *ptable = 0;

    //kmem.lock / vm->lock are also taken by the preemption-timer ISR (via
    //vmem_setactive). Hold interrupts off so this core cannot be preempted while
    //holding them, which would otherwise self-deadlock (or ABBA across cores).
    int cli_state = cli();
    if (virt < 0)
    {
        //Add to the shared kernel map. Writes land directly in the master kernel
        //PML4's upper half and the shared lower-level tables, so the mapping is
        //immediately visible in every address space -- no per-core resync.
        local_spinlock_lock(&kmem.lock);
        ptable = kmem.pml4;
    }
    else
    {
        //Add to user map
        local_spinlock_lock(&vm->lock);
        ptable = vm->pml4;
    }

    int rVal = vmem_map_st(ptable, ptable, virt, phys, size, perms, flags, 0);
    if (virt >= 0)
        local_spinlock_unlock(&vm->lock);
    else
        local_spinlock_unlock(&kmem.lock);
    sti(cli_state);
    return rVal;
}

cs_error vmem_unmap(vmem_t *vm, intptr_t virt, size_t size)
{
    uint64_t *ptable = 0;

    //See vmem_map: interrupts off while holding kmem.lock / vm->lock.
    int cli_state = cli();
    if (virt < 0)
    {
        //Remove from the shared kernel map (see vmem_map). This flushes only the
        //local core (below); a caller unmapping a kernel range at runtime must also
        //call vmem_shootdown(virt, size, -1) afterwards (interrupts on, no lock) to
        //invalidate the other cores before reusing the frame.
        local_spinlock_lock(&kmem.lock);
        ptable = kmem.pml4;
    }
    else
    {
        //Add to user map
        local_spinlock_lock(&vm->lock);
        ptable = vm->pml4;
    }

    int rVal = vmem_unmap_st(ptable, ptable, virt, size, 0);

    //Flush this core's TLB for the range. This is correct for the common
    //self-unmap (the range belongs to the address space active on this core) and
    //harmless otherwise; a *foreign* core caching the range is handled separately
    //by vmem_shootdown, which the caller must invoke before reusing the frame.
    vmem_local_flush(virt, size);

    if (virt >= 0)
        local_spinlock_unlock(&vm->lock);
    else
        local_spinlock_unlock(&kmem.lock);
    sti(cli_state);

    return rVal;
}

int vmem_create(vmem_t **vm_r)
{
    vmem_t *vm = malloc(sizeof(vmem_t));
    if (vm == NULL)
        return -1;

    //Each address space owns a full hardware PML4 page: cr3 points straight at it.
    uintptr_t pml4_phys = physmem_alloc(-1, -1, physmem_alloc_flags_pagetable, KiB(4));
    if (pml4_phys == PHYSMEM_NO_ALLOC)
    {
        free(vm);
        return -1;
    }
    vm->pml4_phys = pml4_phys;
    vm->pml4 = (uint64_t *)vmem_phystovirt(pml4_phys, KiB(4), vmem_flags_cachewriteback);
    vm->flags = vmem_flags_user;
    vm->lock = 0;
    vm->active_apic = -1; //not active on any core until vmem_setactive runs
    memset(vm->pml4, 0, KiB(4));

    //Inherit the shared kernel half. These PML4 entries point at shared
    //lower-level tables, so subsequent kernel-map changes are visible here with no
    //resync (only a brand-new kernel PML4 entry would be missed -- hence all
    //kernel PML4 entries are pre-created in vmem_init).
    int cli_state = cli();
    local_spinlock_lock(&kmem.lock);
    memcpy(vm->pml4 + 256, kmem.pml4 + 256, 256 * sizeof(uint64_t));
    local_spinlock_unlock(&kmem.lock);
    sti(cli_state);

    *vm_r = vm;

    return 0;
}

void vmem_destroy(vmem_t *vm_r)
{
    if (vm_r != NULL)
    {
        int cli_state = cli();
        local_spinlock_lock(&vm_r->lock);
        //Free only this address space's PML4 page. Its kernel-half entries point
        //at shared tables that must NOT be freed; the user-half lower tables are
        //released by vmem_unmap during task teardown.
        physmem_free(vm_r->pml4_phys, KiB(4));
        free(vm_r);
        sti(cli_state);
    }
}

int vmem_setactive(vmem_t *vm)
{
    //With a shared kernel PML4 and each address space owning its own PML4 page,
    //activating one is just a cr3 load -- no page-table state to copy in or out.
    int cli_state = cli();

    //Track which core each address space is active on, so a cross-core unmap can
    //target the right core (tlb_self_apic is NULL until vmem_smp_init, but no task
    //schedules before then, so nothing relies on the mark that early). The cr3
    //load below is a full flush, so a core entering an AS starts with a clean TLB.
    int self = (tlb_self_apic != NULL) ? tlb_self_apic() : -1;
    if (self >= 0)
    {
        vmem_t *prev = lcl->cur_vmem;
        if (prev != NULL && prev != vm)
            __atomic_store_n(&prev->active_apic, -1, __ATOMIC_SEQ_CST);
        __atomic_store_n(&vm->active_apic, self, __ATOMIC_SEQ_CST);
    }

    lcl->cur_vmem = vm;
    __asm__ volatile("mov %0, %%cr3" ::"r"(vm->pml4_phys)
                     :);
    sti(cli_state);

    return 0;
}

int vmem_getactive(vmem_t **vm)
{
    *vm = lcl->cur_vmem;
    return 0;
}

//APIC id of the core a user address space is currently active on, or -1 if none.
//Snapshot this under the same lock that performed the vmem_unmap, then pass the
//value to vmem_shootdown after dropping locks: the vmem_t itself must not be
//dereferenced during the IPI wait (the owning task may exit and free it).
int vmem_active_apic(vmem_t *vm)
{
    if (vm == NULL)
        return -1;
    return __atomic_load_n(&vm->active_apic, __ATOMIC_SEQ_CST);
}

//Shootdown IPI handler -- runs in interrupt context on a *receiving* core. The
//range is published under tlb_lock before the IPI is sent, so a plain read is
//safe. Flush, then acknowledge.
static void tlb_shootdown_handler(int irq)
{
    (void)irq;
    intptr_t v = tlb_virt;
    size_t s = tlb_size;
    vmem_local_flush(v, s);
    __atomic_store_n(&tlb_ack, 1, __ATOMIC_SEQ_CST);
}

//Shoot down one core and wait for it to ack. Caller holds tlb_lock.
static void tlb_send_one(int apic)
{
    __atomic_store_n(&tlb_ack, 0, __ATOMIC_SEQ_CST);
    tlb_sendipi(apic, tlb_vec, ipi_delivery_mode_fixed);
    //One target at a time -- also avoids overrunning the local APIC ICR.
    while (__atomic_load_n(&tlb_ack, __ATOMIC_SEQ_CST) == 0)
        __asm__ volatile("pause");
}

//Complete a cross-core TLB invalidation for a range just unmapped / downgraded by
//vmem_unmap (which already flushed this core). For a kernel range (virt < 0) every
//other core is shot down; for a user range only target_apic is (the core the AS
//was active on, from vmem_active_apic), and only if it is a different core. No-op
//on a single core or before vmem_smp_init. See the contract above: interrupts
//must be enabled and no page-table/task lock may be held.
void vmem_shootdown(intptr_t virt, size_t size, int target_apic)
{
    if (tlb_vec < 0 || tlb_core_count <= 1)
        return; //single core, or SMP shootdown not set up: local flush sufficed

    int self = (tlb_self_apic != NULL) ? tlb_self_apic() : -1;

    local_spinlock_lock(&tlb_lock);
    tlb_virt = virt;
    tlb_size = size;
    if (virt < 0)
    {
        for (int i = 0; i < tlb_core_count; i++)
            if (tlb_all_apics[i] != self)
                tlb_send_one(tlb_all_apics[i]);
    }
    else if (target_apic >= 0 && target_apic != self)
    {
        tlb_send_one(target_apic);
    }
    local_spinlock_unlock(&tlb_lock);
}

//Set up the cross-core TLB-shootdown IPI. Run after SysInterrupts/SysMP are up
//(acpi_init populated HW/LAPIC, intr_init armed the APIC) but before APs start
//scheduling -- i.e. CALL:vmem_smp_init after CALL:mp_init in loadscript.txt.
//SysInterrupts loads after SysVirtualMemory, so its entry points are resolved at
//runtime via the kernel symbol DB rather than linked.
cs_error vmem_smp_init()
{
    cs_error (*intr_allocate)(int, interrupt_flags_t, int *) =
        (cs_error(*)(int, interrupt_flags_t, int *))elf_resolvefunction("interrupt_allocate");
    void (*intr_register)(int, InterruptHandler) =
        (void (*)(int, InterruptHandler))elf_resolvefunction("interrupt_register_handler");
    tlb_self_apic = (int (*)(void))elf_resolvefunction("interrupt_get_cpu_idx");
    tlb_sendipi = (void (*)(int, int, ipi_delivery_mode_t))elf_resolvefunction("interrupt_sendipi");

    if (intr_allocate == NULL || intr_register == NULL || tlb_self_apic == NULL || tlb_sendipi == NULL)
        return CS_UNKN;

    //Enumerate every core's APIC id from the registry (populated from the ACPI
    //MADT at boot, so all cores are known even before the APs are released). The
    //HW/LAPIC/<idx> keys are formatted with a base-16 index (see acpi_tables.c).
    uint64_t count = 0;
    registry_readkey_uint("HW/LAPIC", "COUNT", &count);
    tlb_core_count = 0;
    for (uint64_t i = 0; i < count && tlb_core_count < VMEM_MAX_CORES; i++)
    {
        char idx_str[16] = "";
        char key_str[256] = "HW/LAPIC/";
        char *key = strncat(key_str, itoa((int)i, idx_str, 16), 255);

        uint64_t apic_id = 0;
        if (registry_readkey_uint(key, "APIC ID", &apic_id) != CS_OK)
            continue;
        tlb_all_apics[tlb_core_count++] = (int)apic_id;
    }

    //Allocate a dedicated vector and register the handler once (the handler table
    //is global, so this single registration covers every core's IDT).
    int base = 0;
    if (intr_allocate(1, interrupt_flags_exclusive, &base) != CS_OK)
        return CS_OUTOFMEM;
    intr_register(base, tlb_shootdown_handler);
    tlb_vec = base;
    return CS_OK;
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

cs_error vmem_virttophys(vmem_t *vm, intptr_t virt, intptr_t *phys)
{
    if (virt < 0)
    {
        //kernel address -- walk the shared master kernel PML4
        return vmem_virttophys_st(kmem.pml4, (uint64_t)virt, phys, 0);
    }
    else if (vm != NULL)
    {
        //user address
        return vmem_virttophys_st(vm->pml4, (uint64_t)virt, phys, 0);
    }
    return -2;
}

intptr_t vmem_phystovirt(intptr_t phys, size_t sz, vmem_flags_t flags)
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
    else if (flags & vmem_flags_cachewritecomplete)
    {
        if (phys < (intptr_t)phys_map_sz && (phys + sz) < phys_map_sz)
            return (phys + KERN_PHYSMAP_BASE_WC);
    }

    char tmp[20];
    DEBUG_PRINT(ltoa(phys, tmp, 16));
    PANIC("Invalid Address Detected!");
    return phys;
}