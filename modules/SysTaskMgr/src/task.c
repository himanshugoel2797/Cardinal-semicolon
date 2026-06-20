/**
 * Copyright (c) 2018 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <cardinal/local_spinlock.h>

#include "SysMP/mp.h"
#include "SysFP/fp.h"
#include "SysPhysicalMemory/phys_mem.h"
#include "SysVirtualMemory/vmem.h"
#include "SysMemory/memory.h"
#include "SysUser/syscall.h"
#include "SysTimer/timer.h"
#include "SysInterrupts/interrupts.h"
#include "SysReg/registry.h"

#include "elf.h"
#include "task_priv.h"
#include "error.h"
#include "cs_syscall.h"

#define MAX_CORES 256

// thread/process id allocator
static _Atomic cs_id cur_id = 1;

static _Atomic int process_count = 0;

// Per-core run queues. Shared-nothing scheduling: a task is created, scheduled,
// and freed by a single owning core, so the cross-core use-after-free of an
// exited task's stack/reg_state that the old global run queue raced on is now
// impossible by construction.
//
// These are plain globals (absolute addresses) -- indexed by a core's sequential
// registration index (core_desc_t.core_idx, NOT its sparse APIC id) -- so any
// core may enqueue work onto another core or look a task up by id. The per-core
// core_desc_t itself lives in gs-relative TLS and is only addressable by its
// owner, so the shareable parts of a queue (its head + lock) must live here.
static process_desc_t *run_queues[MAX_CORES] = {0};
static int rq_locks[MAX_CORES] = {0};
static _Atomic int registered_cores = 0; // also the next free run-queue index
static _Atomic int rr_cursor = 0;        // round-robin assignment cursor

// current core description
static TLS core_desc_t *core_descs = NULL;

// The TLS offset of the per-core core_desc_t, allocated ONCE and shared by every
// core. mp_tls_alloc bumps a single global cursor, so it must be called once per
// logical TLS slot -- not once per core. Guarding the call on the (per-core, TLS)
// `core_descs == NULL` would re-run it on every core, handing each core a
// different offset and burning (cores-1) * sizeof(core_desc_t) of the fixed TLS
// block. Instead allocate the offset behind this shared latch; each core then
// resolves the same offset against its own gs base.
static int core_descs_off = -1;
static int core_descs_off_lock = 0;

static TLS core_desc_t *tls_core_descs(void)
{
    local_spinlock_lock(&core_descs_off_lock);
    if (core_descs_off < 0)
        core_descs_off = mp_tls_alloc(sizeof(core_desc_t));
    int off = core_descs_off;
    local_spinlock_unlock(&core_descs_off_lock);
    return (TLS core_desc_t *)mp_tls_get(off);
}

// Insert a freshly-built task into core `idx`'s run queue.
static void rq_insert(int idx, process_desc_t *t)
{
    int cli_state = cli();
    local_spinlock_lock(&rq_locks[idx]);
    t->owner_core = idx;
    t->next = run_queues[idx];
    run_queues[idx] = t;
    local_spinlock_unlock(&rq_locks[idx]);
    sti(cli_state);
}

// Choose a core to host a newly-created task. Round-robins across the cores that
// have come online so far. While booting single-core (registered_cores == 1)
// everything lands on the BSP, which is required: the boot/servicescript task
// must run on the BSP, and APs only begin pulling work once they register.
static int pick_target_core(void)
{
    int n = registered_cores;
    if (n <= 1)
        return 0;
    return (rr_cursor++) % n;
}

// Find a task by id across every core's run queue. On success returns the task
// with its ->lock held (caller must unlock); returns NULL if no such id exists.
// At most one rq_lock is held at a time, and always acquired before ->lock, so
// this cannot deadlock against a core's scheduler (which locks its own rq_lock,
// then a task ->lock).
//
// PRECONDITION: the caller must have interrupts disabled (cli()). This routine
// takes rq_locks[i]; if a timer tick fired on this core while we held our own
// core's rq_lock, task_switch_handler would spin on that same lock with no way
// to make progress -> per-core self-deadlock. All callers cli() first; the
// assert below catches any future caller that forgets.
static process_desc_t *find_task_locked(cs_id id)
{
    // cli() returns the prior IF state and disables interrupts. If interrupts
    // were enabled on entry the precondition is violated; restore and panic.
    int irqs_were_on = cli();
    if (irqs_were_on)
    {
        sti(irqs_were_on);
        PANIC("[SysTaskMgr] find_task_locked called with interrupts enabled.");
    }

    int n = registered_cores;
    for (int i = 0; i < n; i++)
    {
        local_spinlock_lock(&rq_locks[i]);
        for (process_desc_t *it = run_queues[i]; it != NULL; it = it->next)
        {
            if (it->id == id)
            {
                local_spinlock_lock(&it->lock);
                local_spinlock_unlock(&rq_locks[i]);
                return it;
            }
        }
        local_spinlock_unlock(&rq_locks[i]);
    }
    return NULL;
}

static cs_error create_task_core(const char *name, task_permissions_t perms, cs_id *id, int target_core)
{
    cs_id alloc_id = cur_id++;

    //Create the process address space and add it to the list
    process_desc_t *proc_info = malloc(sizeof(process_desc_t));

    DEBUG_PRINT("[SysTaskMgr] Process Created: ");
    DEBUG_PRINT(name);
    DEBUG_PRINT("\r\n");
    if (proc_info == NULL)
    {
        free(proc_info);
        return CS_OUTOFMEM;
    }

    memset(proc_info, 0, sizeof(process_desc_t));
    strncpy(proc_info->name, name, 256);
    proc_info->id = alloc_id;
    proc_info->lock = 0;

    if (vmem_create(&proc_info->mem) != 0)
    {
        free(proc_info);
        return CS_OUTOFMEM;
    }

    *id = alloc_id;

    proc_info->state = task_state_uninitialized;
    proc_info->permissions = perms;

    //Allocate the kernel level stack
    proc_info->kernel_stack = malloc(KERNEL_STACK_LEN);
    if (proc_info->kernel_stack == NULL)
        PANIC("[SysTaskMgr] Unexpected memory allocation failure.");
    proc_info->kernel_stack += KERNEL_STACK_LEN;

    proc_info->user_stack = NULL;
    proc_info->syscall_data = NULL;

    if (perms == task_permissions_none)
    {
        proc_info->syscall_data = malloc(syscall_getfullstate_size());
        syscall_getdefaultstate(proc_info->syscall_data, proc_info->kernel_stack, proc_info->user_stack, NULL);
    }

    proc_info->fpu_state = malloc(fp_platform_getstatesize() + fp_platform_getalign());
    proc_info->fpu_state_unaligned = proc_info->fpu_state;
    if (proc_info->fpu_state == NULL)
        PANIC("[SysTaskMgr] Unexpected memory allocation failure.");

    //Align the FPU state properly
    if ((uintptr_t)proc_info->fpu_state % fp_platform_getalign() != 0)
        proc_info->fpu_state += fp_platform_getalign() - ((uintptr_t)proc_info->fpu_state % fp_platform_getalign());

    fp_platform_getdefaultstate(proc_info->fpu_state);

    proc_info->reg_state = malloc(mp_platform_getstatesize());
    if (proc_info->reg_state == NULL)
        PANIC("[SysTaskMgr] Unexpected memory allocation failure.");
    mp_platform_getdefaultstate(proc_info->reg_state, proc_info->kernel_stack, NULL, NULL, NULL);

    //Assign to a core's run queue
    if (target_core < 0)
        target_core = pick_target_core();
    rq_insert(target_core, proc_info);

    process_count++;

    return CS_OK;
}

cs_error task_create_kernel(const char *name, task_permissions_t perms, cs_id *id)
{
    return create_task_core(name, perms, id, -1);
}

//Create a kernel task pinned to a specific core (its sequential run-queue index,
//0..task_corecount()-1 -- NOT the sparse APIC id). Used by SysTest to fan a
//per-CPU test onto every online core. An out-of-range core falls back to the
//normal round-robin placement.
cs_error task_create_kernel_oncore(const char *name, task_permissions_t perms, int core, cs_id *id)
{
    if (core < 0 || core >= registered_cores)
        core = -1;
    return create_task_core(name, perms, id, core);
}

//Number of cores that have joined the scheduler so far (each owns run-queue
//index 0..task_corecount()-1). Grows as APs come online after task_release_aps.
int task_corecount(void)
{
    return registered_cores;
}

static void NORETURN kernel_entry_handler(void *handler, void *arg)
{
    ((void(*)(void*))handler)(arg);
    task_end_kernel(task_current());
    while(true)
        task_yield();
}

cs_error task_start_kernel(cs_id id, void *handler, void *arg)
{
    if (handler != NULL)
    {
        int cli_state = cli();
        process_desc_t *iter = find_task_locked(id);
        if (iter != NULL)
        {
            //Lock is held from find_task_locked
            DEBUG_PRINT("[SysTaskMgr] Process Started: ");
            DEBUG_PRINT(iter->name);
            DEBUG_PRINT("\r\n");

            if (iter->permissions == task_permissions_none)
            {
                iter->user_stack = (uint8_t *)0x100000000;
                iter->user_stack += USER_STACK_LEN - sizeof(struct cardinal_program_setup_params);

                uintptr_t pmem = physmem_alloc(0, 0, physmem_alloc_flags_data | physmem_alloc_flags_zero, USER_STACK_LEN);
                if (pmem == PHYSMEM_NO_ALLOC)
                    PANIC("[SysTaskMgr] Out of memory allocating user stack.");
                iter->user_stack_phys = pmem;
                vmem_map(iter->mem, (intptr_t)0x100000000, (intptr_t)pmem, USER_STACK_LEN, vmem_flags_cachewriteback | vmem_flags_rw | vmem_flags_user, 0);

                iter->usersetup_params = (struct cardinal_program_setup_params *)vmem_phystovirt(iter->user_stack_phys + USER_STACK_LEN - sizeof(struct cardinal_program_setup_params), sizeof(struct cardinal_program_setup_params), vmem_flags_cachewriteback | vmem_flags_rw);
                iter->usersetup_params->ver = 1;
                iter->usersetup_params->page_size = KiB(4);
                iter->usersetup_params->argc = 0;
                iter->usersetup_params->pid = iter->id;
                iter->usersetup_params->rng_seed = 0;
                iter->usersetup_params->entry_point = (uintptr_t)handler;
                iter->usersetup_params->envp = NULL;
                iter->usersetup_params->argv = NULL;

                //setup userspace transition
                syscall_getdefaultstate(iter->syscall_data, iter->kernel_stack, iter->user_stack, (void *)handler);
                mp_platform_getdefaultstate(iter->reg_state, iter->kernel_stack, (void *)syscall_touser, iter->user_stack, NULL); //Rebuild stack state
            }
            else
            {
                mp_platform_getdefaultstate(iter->reg_state, iter->kernel_stack, (void*)kernel_entry_handler, handler, arg); //Rebuild stack state
            }
            iter->state = task_state_pending; //Set task to initialized

            local_spinlock_unlock(&iter->lock);
            sti(cli_state);
            return CS_OK;
        }
        sti(cli_state);
    }
    return CS_UNKN;
}

cs_error task_end_kernel(cs_id id)
{
    int cli_state = cli();
    process_desc_t *iter = find_task_locked(id);
    if (iter != NULL)
    {
        //Lock is held from find_task_locked
        DEBUG_PRINT("[SysTaskMgr] Process Exited: ");
        DEBUG_PRINT(iter->name);
        DEBUG_PRINT("\r\n");

        iter->state = task_state_exited; //Set task to exited

        local_spinlock_unlock(&iter->lock);
        sti(cli_state);
        return CS_OK;
    }
    sti(cli_state);
    return CS_UNKN;
}

cs_error task_create_syscall(char *name, cs_id *id)
{
    return task_create_kernel(name, task_permissions_none, id);
}

cs_error task_start_syscall(cs_id id, void (*handler)(void *arg), void *arg)
{
    return task_start_kernel(id, handler, arg);
}

cs_error task_end_syscall()
{
    int cli_state = cli();
    cs_error retVal = task_end_kernel(task_current());
    sti(cli_state);

    if (retVal == CS_OK)
        while (1)
            task_yield();
    return retVal;
}

cs_error openspecialset_syscall(uint32_t set_id, uint32_t *call_idx)
{
    int cli_state = cli();
    syscall_set_syscallset(set_id, NULL);   //TODO: Add a global table of syscall sets to enable/disable
    *call_idx = set_id;                     //TODO: This can change based on syscall set allocation scheme
    sti(cli_state);
    return CS_OK;
}

static cs_id alloc_descriptor(process_desc_t *pinfo, descriptor_type_t ntype)
{
    cs_id id = 0;

    descriptor_entry_t *d = pinfo->descriptors;

    for (int i = 0; i < MAX_DESCRIPTOR_COUNT; i++)
    {
        if (i == MAX_DESCRIPTOR_COUNT - 1 && d[i].type == descriptor_type_unused_entry)
        {
            //Allocate a new descriptor table
            d[i].type = descriptor_type_descriptor_entry;
            d[i].desc_entry = malloc(sizeof(descriptor_entry_t) * MAX_DESCRIPTOR_COUNT);
            memset(d[i].desc_entry, 0, sizeof(descriptor_entry_t) * MAX_DESCRIPTOR_COUNT);
        }
        if (d[i].type == descriptor_type_descriptor_entry)
        {
            //Follow the chain pointer (always the last slot) into the sub-table.
            //This table held i real descriptors (indices 0..i-1), so the
            //sub-table's ids continue at id + i. Advance id and follow d[i]
            //BEFORE resetting i -- doing i=-1 first would read d[-1] (OOB) and
            //add -1 to id.
            id += i;
            d = d[i].desc_entry;
            i = -1;  //the loop's i++ makes this 0
            continue;
        }
        if (d[i].type == descriptor_type_unused_entry)
        {
            //Valid entry found
            d[i].type = ntype;
            return id + i;
        }
    }

    PANIC("[SysTaskMgr] Descriptor allocation failed.");
    return -1;
}

static descriptor_entry_t *read_descriptor(process_desc_t *pinfo, cs_id id)
{
    cs_id base_id = 0;
    descriptor_entry_t *d = pinfo->descriptors;

    for (int i = 0; i < MAX_DESCRIPTOR_COUNT; i++)
    {
        if (d[i].type == descriptor_type_descriptor_entry)
        {
            //Follow the chain pointer (always the last slot) into the sub-table.
            //Advance base_id by the i real descriptors in this table and follow
            //d[i] BEFORE resetting i -- doing i=-1 first would read d[-1] (OOB).
            base_id += i;
            d = d[i].desc_entry;
            i = -1;  //the loop's i++ makes this 0
            continue;
        }
        if ((d[i].type != descriptor_type_unused_entry) && (base_id + i == id))
        {
            //Valid entry found
            return &d[i];
        }
    }

    PANIC("[SysTaskMgr] Descriptor read failed.");
    return NULL;
}

static void
free_descriptors(process_desc_t *pinfo, descriptor_entry_t *desc_table, cs_id base_id)
{
    if ((pinfo == NULL) | (desc_table == NULL))
        PANIC("[SysTaskMgr] Bad arguments to free_descriptors, memory may be corrupted.");

    for (int i = 0; i < MAX_DESCRIPTOR_COUNT; i++)
    {
        switch (desc_table[i].type)
        {
        case descriptor_type_map_entry:
        {
            task_unmap(pinfo->id, base_id + i);
            desc_table[i].desc_entry = NULL;
            desc_table[i].type = descriptor_type_unused_entry;
        }
        break;
        case descriptor_type_resource_entry:
        {
            task_freedescriptor(pinfo->id, base_id + i);
        }
        break;
        case descriptor_type_descriptor_entry:
        {
            if (desc_table[i].desc_entry != NULL)
            {
                free_descriptors(pinfo, desc_table[i].desc_entry, base_id + i);
                free(desc_table[i].desc_entry);
                desc_table[i].desc_entry = NULL;
                desc_table[i].type = descriptor_type_unused_entry;
            }
        }
        break;
        default:
            break;
        }
    }
}

//Free a task this core retired on a previous scheduler pass. Caller holds
//rq_locks[self]. By now the core has switched onto another task's stack, so
//reclaiming this task's kernel stack/reg_state/struct is safe. A task is only
//ever created, scheduled, and freed by one owning core, so no other core can be
//mid-iret on this stack -- the use-after-free this code used to race on is now
//structurally impossible.
static void free_task(int self, process_desc_t *t)
{
    //Unlink from this core's run queue.
    if (run_queues[self] == t)
        run_queues[self] = t->next;
    else
    {
        for (process_desc_t *p = run_queues[self]; p != NULL; p = p->next)
            if (p->next == t)
            {
                p->next = t->next;
                break;
            }
    }

    //The unlink above ran under rq_locks[self], and every cross-core management
    //call (task_sleep/task_monitor/...) can only reach this task through
    //find_task_locked, which must hold rq_locks[self] to find it -- so once we
    //have unlinked it under that lock, no NEW holder of t->lock can appear. A
    //holder that found t just before the unlink may still be mid-call; acquiring
    //t->lock here drains that last in-flight holder. After this point t is on no
    //queue and unreachable, so it is the final owner of the lock.
    local_spinlock_lock(&t->lock);

    if (t->mem != NULL)
    {
        free_descriptors(t, t->descriptors, 0); //Unmap/free all descriptor regions
        if (t->syscall_data != NULL)
            free(t->syscall_data);
        if (t->user_stack != NULL)
        {
            //No vmem_shootdown needed here: t has exited, so its address space is
            //no longer in any core's cr3 (the retiring core did a full cr3 flush
            //switching away, and active_apic is -1). If a "destroy a live AS" path
            //is ever added, it MUST shoot down before this physmem_free (see
            //task_unmap) or another core could UAF the freed frame via a stale TLB.
            vmem_unmap(t->mem, 0x100000000, USER_STACK_LEN);
            physmem_free(t->user_stack_phys, USER_STACK_LEN);
        }
        vmem_destroy(t->mem);
        free(t->fpu_state_unaligned);
        free(t->reg_state);
        free(t->kernel_stack - KERNEL_STACK_LEN);
        t->mem = NULL;
    }

    //We never unlock t->lock: the struct is freed here and, per the unlink
    //argument above, no other core can still hold or be waiting on it. The lock
    //simply dies with the allocation.
    free(t);
    process_count--;
}

//Reap tasks that were ended (end_task) while NOT running -- i.e. exited while
//pending/sleeping in this core's queue. The prev_dead path only frees a task
//that exits while it is cur_task; an exited pending task is never re-selected
//(task_runnable is false for it) and would otherwise leak and clog the queue
//forever. Such a task is on no core's kernel stack (it was not running), so it
//is safe to free immediately. `keep` is this core's cur_task, handled separately
//via prev_dead -- never freed here. Caller holds rq_locks[self].
static void reap_exited(int self, process_desc_t *keep)
{
    process_desc_t *t = run_queues[self];
    while (t != NULL)
    {
        process_desc_t *next = t->next;  //free_task unlinks t but leaves next valid
        if (t != keep && t->state == task_state_exited)
            free_task(self, t);
        t = next;
    }
}

static bool task_runnable(process_desc_t *t)
{
    //Caller holds rq_locks[self]. We briefly take t->lock so that state and the
    //state-discriminated union member (monitor_tgt / sleep_end share storage)
    //are read as a consistent pair. Writers on another core mutate both under
    //t->lock -- task_sleep/task_monitor reach into this queue via
    //find_task_locked -- so without the lock this is a C11 data race and, worse,
    //a stale `state` could be paired with the other union member's bytes. Lock
    //order rq -> task matches find_task_locked, so it cannot deadlock; the
    //scheduler has already released cur->lock before calling select_next.
    local_spinlock_lock(&t->lock);
    bool runnable;
    switch (t->state)
    {
    case task_state_pending:
        runnable = true;
        break;
    case task_state_suspended_monitor_mem_32:
        runnable = (*t->monitor_tgt != t->monitor_value);
        break;
    case task_state_sleep:
        runnable = (timer_timestamp_ns() >= t->sleep_end);
        break;
    default:
        runnable = false;
        break;
    }
    local_spinlock_unlock(&t->lock);
    return runnable;
}

//Pick the next runnable task from this core's run queue: first the tasks after
//the just-run one (round-robin fairness), then a full scan from the head. Caller
//holds rq_locks[self]. Never returns NULL in practice -- every core owns an
//always-runnable idle task.
static process_desc_t *select_next(int self, process_desc_t *after)
{
    process_desc_t *t = (after != NULL) ? after->next : run_queues[self];
    while (t != NULL)
    {
        if (task_runnable(t))
            return t;
        t = t->next;
    }
    for (t = run_queues[self]; t != NULL; t = t->next)
        if (task_runnable(t))
            return t;
    return NULL;
}

static void task_switch_handler(int irq)
{
    irq = 0;

    int cli_state = cli();
    int self = core_descs->core_idx;
    local_spinlock_lock(&rq_locks[self]);

    //Free the task we retired on the previous pass; we have since switched onto a
    //different task's stack, so this is safe.
    if (core_descs->prev_dead != NULL)
    {
        free_task(self, core_descs->prev_dead);
        core_descs->prev_dead = NULL;
    }

    process_desc_t *cur = core_descs->cur_task;
    if (cur != NULL)
    {
        local_spinlock_lock(&cur->lock);

        if (cur->state == task_state_exited)
        {
            //Switching away from an exited task. We are still on its kernel stack
            //(the interrupt epilogue + iret run after this returns), so we cannot
            //free it yet. Defer to the next pass, by which point this core has
            //iret'd onto the next task's stack. Do not save its state -- it is dead.
            core_descs->prev_dead = cur;
            local_spinlock_unlock(&cur->lock);
        }
        else
        {
            if (cur->state == task_state_running)
                cur->state = task_state_pending;  //Set the cur_task to pending again
            fp_platform_getstate(cur->fpu_state); //Save the current task's fpu state
            mp_platform_getstate(cur->reg_state); //Save the current task's register state
            if (cur->syscall_data != NULL)
                syscall_getfullstate(cur->syscall_data);
            local_spinlock_unlock(&cur->lock);
        }
    }

    reap_exited(self, cur);  //free tasks ended while pending/sleeping (not cur)

    process_desc_t *ntask = select_next(self, cur);
    if (ntask == NULL)
        PANIC("[SysTaskMgr] Out of Processes!\r\n");

    //switch to this task
    core_descs->cur_task = ntask;

    local_spinlock_lock(&ntask->lock);
    ntask->state = task_state_running;
    vmem_setactive(ntask->mem);             //Set virtual memory
    fp_platform_setstate(ntask->fpu_state); //Set fpu state
    mp_platform_setstate(ntask->reg_state); //Set registers
    if (ntask->syscall_data != NULL)
        syscall_setfullstate(ntask->syscall_data);
    local_spinlock_unlock(&ntask->lock);

    local_spinlock_unlock(&rq_locks[self]);
    sti(cli_state);
}

static void task_yield_stage2(interrupt_register_state_t *mp_state){
    int self = core_descs->core_idx;
    local_spinlock_lock(&rq_locks[self]);

    //Free the task we retired on the previous pass; we have since switched onto a
    //different task's stack, so this is safe.
    if (core_descs->prev_dead != NULL)
    {
        free_task(self, core_descs->prev_dead);
        core_descs->prev_dead = NULL;
    }

    process_desc_t *cur = core_descs->cur_task;
    if (cur != NULL)
    {
        local_spinlock_lock(&cur->lock);

        if (cur->state == task_state_exited)
        {
            //Yielding out of an exited task (e.g. the tail of task_end_syscall).
            //task_yield restores registers and irets off this same kernel stack
            //after we return, so we cannot free it yet. Defer to the next pass;
            //skip the save -- it is dead.
            core_descs->prev_dead = cur;
            local_spinlock_unlock(&cur->lock);
        }
        else
        {
            if (cur->state == task_state_running)
                cur->state = task_state_pending;  //Set the cur_task to pending again
            fp_platform_getstate(cur->fpu_state); //Save the current task's fpu state
            memcpy(cur->reg_state, mp_state, sizeof(interrupt_register_state_t)); //Save the current task's register state
            if (cur->syscall_data != NULL)
                syscall_getfullstate(cur->syscall_data);
            local_spinlock_unlock(&cur->lock);
        }
    }

    reap_exited(self, cur);  //free tasks ended while pending/sleeping (not cur)

    process_desc_t *ntask = select_next(self, cur);
    if (ntask == NULL)
        PANIC("[SysTaskMgr] Out of Processes!\r\n");

    //switch to this task
    core_descs->cur_task = ntask;

    local_spinlock_lock(&ntask->lock);
    ntask->state = task_state_running;
    vmem_setactive(ntask->mem);             //Set virtual memory
    fp_platform_setstate(ntask->fpu_state); //Set fpu state
    //mp_platform_setstate(ntask->reg_state); //Set registers
    memcpy(mp_state, ntask->reg_state, sizeof(interrupt_register_state_t));
    if (ntask->syscall_data != NULL)
        syscall_setfullstate(ntask->syscall_data);
    local_spinlock_unlock(&ntask->lock);

    local_spinlock_unlock(&rq_locks[self]);
}

void task_yield(){
    //turn off interrupts
    interrupt_register_state_t state;
    int cli_state = cli();
    //save state
    //register uint64_t rax __asm__("rax") = (uint64_t)&state;
    __asm__ volatile(
        "mov %%r15, (%%rax)\r\n"
        "mov %%r14, 0x8(%%rax)\r\n"
        "mov %%r13, 0x10(%%rax)\r\n"
        "mov %%r12, 0x18(%%rax)\r\n"
        "mov %%r11, 0x20(%%rax)\r\n"
        "mov %%r10, 0x28(%%rax)\r\n"
        "mov %%r9, 0x30(%%rax)\r\n"
        "mov %%r8, 0x38(%%rax)\r\n"
        "mov %%rdi, 0x40(%%rax)\r\n"
        "mov %%rsi, 0x48(%%rax)\r\n"
        "mov %%rdx, 0x50(%%rax)\r\n"
        "mov %%rcx, 0x58(%%rax)\r\n"
        "mov %%rbx, 0x60(%%rax)\r\n"
        "movq %%rax, 0x68(%%rax)\r\n"

        //rflags
        "pushfq\r\n"
        "pop %%rbx\r\n"
        "mov %%rbx, 0x70(%%rax)\r\n"

        //rip
        "movabsq $resume_from_yield, %%rbx\r\n"
        "mov %%rbx, 0x78(%%rax)\r\n"

        //cs
        "mov %%cs, 0x80(%%rax)\r\n"

        //ss
        "mov %%ss, 0x88(%%rax)\r\n"

        "mov %%rbp, 0x90(%%rax)\r\n"
        "mov %%rsp, 0x98(%%rax)\r\n"
        ::"a"(&state): "rbx"
    );
    //find new task
    task_yield_stage2(&state);

    //restore state
    __asm__ volatile(
        "mov (%%rax), %%r15\r\n"
        "mov 0x8(%%rax), %%r14\r\n"
        "mov 0x10(%%rax), %%r13\r\n"
        "mov 0x18(%%rax), %%r12\r\n"
        "mov 0x20(%%rax), %%r11\r\n"
        "mov 0x28(%%rax), %%r10\r\n"
        "mov 0x30(%%rax), %%r9\r\n"
        "mov 0x38(%%rax), %%r8\r\n"
        "mov 0x40(%%rax), %%rdi\r\n"
        "mov 0x48(%%rax), %%rsi\r\n"
        "mov 0x50(%%rax), %%rdx\r\n"
        "mov 0x58(%%rax), %%rcx\r\n"
        "mov 0x60(%%rax), %%rbx\r\n"
        "mov 0x88(%%rax), %%rbp\r\n" //push ss, using rbp as a temp
        "push %%rbp\r\n"
        "mov 0x98(%%rax), %%rbp\r\n" //push rsp, using rbp as a temp
        "push %%rbp\r\n"
        "mov 0x70(%%rax), %%rbp\r\n" //push rflags, using rbp as a temp
        "push %%rbp\r\n"
        "mov 0x80(%%rax), %%rbp\r\n" //push cs, using rbp as a temp
        "push %%rbp\r\n"
        "mov 0x78(%%rax), %%rbp\r\n" //push rip, using rbp as a temp
        "push %%rbp\r\n"
        "mov 0x90(%%rax), %%rbp\r\n" //correct rbp
        "mov 0x68(%%rax), %%rax\r\n" //correct rax
        "iretq\r\n"
    ::"a"(&state):);

    __asm__ volatile("resume_from_yield:\r\n");
    //turn on interrupts
    sti(cli_state);
    return;
}

cs_error task_virttophys(cs_id id, intptr_t vaddr, intptr_t *phys)
{
    if (phys == NULL)
        return CS_UNKN;

    int cli_state = cli();
    cs_error res_cs = CS_UNKN;
    process_desc_t *iter = find_task_locked(id);
    if (iter != NULL)
    {
        int res = vmem_virttophys(iter->mem, vaddr, phys);
        if (res == 0)
            res_cs = CS_OK;

        local_spinlock_unlock(&iter->lock);
    }
    sti(cli_state);
    return res_cs;
}

cs_id task_current()
{
    return core_descs->cur_task->id;
}

cs_error task_monitor_noyield(cs_id id, uint32_t *tgt, uint32_t cur_val)
{
    if (tgt == NULL)
        return CS_UNKN;

    int cli_state = cli();
    cs_error res_cs = CS_UNKN;
    process_desc_t *iter = find_task_locked(id);
    if (iter != NULL)
    {
        intptr_t phys_ptr = 0;
        vmem_virttophys(iter->mem, (intptr_t)tgt, &phys_ptr);
        iter->monitor_tgt = (uint32_t*)vmem_phystovirt(phys_ptr, 4, vmem_flags_uncached | vmem_flags_kernel);
        iter->monitor_value = cur_val;
        iter->state = task_state_suspended_monitor_mem_32;

        local_spinlock_unlock(&iter->lock);
    }
    sti(cli_state);
    return res_cs;
}

cs_error task_monitor(cs_id id, uint32_t *tgt, uint32_t cur_val)
{
    if (tgt == NULL)
        return CS_UNKN;

    int cli_state = cli();
    task_monitor_noyield(id, tgt, cur_val);
    task_yield();
    sti(cli_state);
    return CS_UNKN;
}

cs_error task_map(cs_id id, const char *name, intptr_t vaddr, size_t sz, task_map_flags_t flags, task_map_perms_t owner_perms, task_map_perms_t child_perms, int child_count, cs_id *shmem_id)
{
    if (shmem_id == NULL)
        return CS_UNKN;

    if (name == NULL && (flags & task_map_shared) != 0)  //shared mappings require a name
        return CS_UNKN;

    if ((flags & task_map_oneway) != 0 && (flags & task_map_shared) == 0) //oneway mapping is only valid with shared memory
        return CS_UNKN;

    if ((flags & task_map_oneuse) != 0 && (flags & task_map_shared) == 0) //oneuse mapping is only valid with shared memory
        return CS_UNKN;

    if ((flags & task_map_shared) != 0)
        PANIC("[SysTaskMgr] Shared memory not implemented.");

    int cli_state = cli();
    process_desc_t *iter = find_task_locked(id);
    if (iter != NULL)
    {
        cs_id shmem_k_id = alloc_descriptor(iter, descriptor_type_map_entry);
        descriptor_entry_t *d = read_descriptor(iter, shmem_k_id);
        //Map memory region
        d->type = descriptor_type_map_entry;
        d->map_entry = malloc(sizeof(map_entry_t));
        d->map_entry->vaddr = vaddr;
        d->map_entry->paddr = 0;
        d->map_entry->sz = sz;
        d->map_entry->owner_perms = owner_perms;
        d->map_entry->child_perms = child_perms;
        d->map_entry->flags = flags;
        d->map_entry->child_count = child_count;

        if ((flags & task_map_shared) != 0)
        {
            if ((flags & task_map_oneway) != 0)
            {
                //oneway
            }

            if ((flags & task_map_oneuse) != 0)
            {
                //oneuse - unmapping doesn't release allowed map count
            }
            PANIC("[SysTaskMgr] Shared memory not implemented.");
            //TODO: handle shared memory tree
        }
        else
        {
            int map_perms = 0;
            if (owner_perms & task_map_perm_writeonly)
                map_perms |= vmem_flags_rw;
            if (owner_perms & task_map_perm_execute)
                map_perms |= vmem_flags_exec;
            if (owner_perms & task_map_perm_cachewritethrough)
                map_perms |= vmem_flags_cachewritethrough;
            else if (owner_perms & task_map_perm_cachewriteback)
                map_perms |= vmem_flags_cachewriteback;
            else if (owner_perms & task_map_perm_cachewritecomplete)
                map_perms |= vmem_flags_cachewritecomplete;
            else if (owner_perms & task_map_perm_uncached)
                map_perms |= vmem_flags_uncached;

            if (iter->permissions & task_permissions_kernel)
                map_perms |= vmem_flags_kernel;
            else
                map_perms |= vmem_flags_user;

            //Allocate physical memory and map it into the process
            uintptr_t pmem = physmem_alloc(0, 0, physmem_alloc_flags_data | physmem_alloc_flags_instr | physmem_alloc_flags_zero, sz);
            if (pmem == PHYSMEM_NO_ALLOC)
                PANIC("[SysTaskMgr] Out of memory allocating process image.");
            d->map_entry->paddr = pmem;
            d->map_entry->is_owner = true;
            vmem_map(iter->mem, d->map_entry->vaddr, (intptr_t)pmem, sz, map_perms, 0);
        }

        *shmem_id = shmem_k_id;
        local_spinlock_unlock(&iter->lock);
    }
    sti(cli_state);
    return CS_OK;
}

cs_error task_updatemap(cs_id id, cs_id shmem_id, task_map_perms_t perms)
{
    bool do_shootdown = false;
    intptr_t fl_vaddr = 0;
    size_t fl_sz = 0;
    int fl_apic = -1;

    int cli_state = cli();
    process_desc_t *iter = find_task_locked(id);
    if (iter != NULL)
    {
        descriptor_entry_t *d = read_descriptor(iter, shmem_id);
        if (d->type == descriptor_type_map_entry)
        {
            //Remap memory region
            if (d->map_entry->is_owner)
                perms &= d->map_entry->owner_perms;
            else
                perms &= d->map_entry->child_perms;

            int map_perms = 0;
            if (perms & task_map_perm_writeonly)
                map_perms |= vmem_flags_rw;
            if (perms & task_map_perm_execute)
                map_perms |= vmem_flags_exec;
            if (perms & task_map_perm_cachewritethrough)
                map_perms |= vmem_flags_cachewritethrough;
            else if (perms & task_map_perm_cachewriteback)
                map_perms |= vmem_flags_cachewriteback;
            else if (perms & task_map_perm_cachewritecomplete)
                map_perms |= vmem_flags_cachewritecomplete;
            else if (perms & task_map_perm_uncached)
                map_perms |= vmem_flags_uncached;

            if (iter->permissions & task_permissions_kernel)
                map_perms |= vmem_flags_kernel;
            else
                map_perms |= vmem_flags_user;

            //Remap with the (possibly reduced) permissions. The local core is
            //flushed by vmem_unmap; a foreign core that still caches the old, more
            //permissive entry is handled by the cross-core shootdown below, after
            //iter->lock is dropped. Capture the range + active core under the lock.
            fl_vaddr = d->map_entry->vaddr;
            fl_sz = d->map_entry->sz;
            vmem_unmap(iter->mem, fl_vaddr, fl_sz);
            vmem_map(iter->mem, fl_vaddr, (intptr_t)d->map_entry->paddr, fl_sz, map_perms, 0);
            fl_apic = vmem_active_apic(iter->mem);
            do_shootdown = true;
        }
        local_spinlock_unlock(&iter->lock);
    }
    sti(cli_state);

    //Interrupts on, no lock held (see vmem_shootdown). Ensures the reduced
    //permissions are enforced on every core, not just the one that edited.
    if (do_shootdown)
        vmem_shootdown(fl_vaddr, fl_sz, fl_apic);

    return CS_OK;
}

cs_error task_unmap(cs_id id, cs_id shmem_id)
{
    bool do_shootdown = false;
    bool free_phys = false;
    intptr_t fl_vaddr = 0;
    size_t fl_sz = 0;
    int fl_apic = -1;
    uintptr_t fl_paddr = 0;

    int cli_state = cli();
    process_desc_t *iter = find_task_locked(id);
    if (iter != NULL)
    {
        descriptor_entry_t *d = read_descriptor(iter, shmem_id);
        if (d->type == descriptor_type_map_entry)
        {
            //Unmap (this also flushes the local core's TLB) and capture what the
            //cross-core shootdown and the physical free -- both deferred until
            //after we drop iter->lock -- will need. The shootdown must complete
            //before the frame is returned for reuse, and vmem_active_apic must be
            //read while the address space is still pinned by iter->lock.
            fl_vaddr = d->map_entry->vaddr;
            fl_sz = d->map_entry->sz;
            vmem_unmap(iter->mem, fl_vaddr, fl_sz);
            fl_apic = vmem_active_apic(iter->mem);
            do_shootdown = true;

            if (d->map_entry->is_owner)
            {
                free_phys = true;
                fl_paddr = d->map_entry->paddr;
            }

            free(d->map_entry);
            d->map_entry = NULL;
            d->type = descriptor_type_unused_entry;
        }
        local_spinlock_unlock(&iter->lock);
    }
    sti(cli_state);

    //vmem_shootdown busy-waits for the target core to ack from interrupt context,
    //so it must run with interrupts on and no lock held; it must also precede the
    //physmem_free so no core can touch the frame through a stale TLB entry once it
    //is reusable.
    if (do_shootdown)
        vmem_shootdown(fl_vaddr, fl_sz, fl_apic);
    if (free_phys)
        physmem_free(fl_paddr, fl_sz);

    return CS_OK;
}

cs_error task_allocdescriptor(cs_id id, DescriptorResourceFreeAction action, void *state NULLABLE, cs_id *descriptor NULLABLE)
{
    if (action == NULL)
        return CS_UNKN;

    int cli_state = cli();
    process_desc_t *iter = find_task_locked(id);
    if (iter != NULL)
    {
        cs_id shmem_k_id = alloc_descriptor(iter, descriptor_type_resource_entry);
        descriptor_entry_t *d = read_descriptor(iter, shmem_k_id);
        //Map memory region
        d->type = descriptor_type_resource_entry;
        d->resource_entry = malloc(sizeof(resource_entry_t));
        d->resource_entry->action = action;
        d->resource_entry->state = state;

        if (descriptor != NULL)
            *descriptor = shmem_k_id;
        local_spinlock_unlock(&iter->lock);
    }
    sti(cli_state);
    return CS_OK;
}

cs_error task_freedescriptor(cs_id id, cs_id descriptor)
{
    int cli_state = cli();
    process_desc_t *iter = find_task_locked(id);
    if (iter != NULL)
    {
        descriptor_entry_t *d = read_descriptor(iter, descriptor);
        if (d->type == descriptor_type_resource_entry)
        {
            //Free the associated resource
            d->resource_entry->action(d->resource_entry->state);

            free(d->resource_entry);
            d->resource_entry = NULL;
            d->type = descriptor_type_unused_entry;
        }
        local_spinlock_unlock(&iter->lock);
    }
    sti(cli_state);
    return CS_OK;
}

cs_error task_sleep(cs_id id, uint64_t ns)
{
    int cli_state = cli();
    process_desc_t *iter = find_task_locked(id);
    if (iter == NULL)
    {
        //No such task: nothing to put to sleep, and yielding here would be
        //wrong (it would deschedule whatever unrelated task is running now).
        sti(cli_state);
        return CS_UNKN;
    }

    iter->sleep_end = timer_timestamp_ns() + ns;
    iter->state = task_state_sleep;
    //Whether we just put *this core's* running task to sleep. Captured under
    //cli() before we drop the lock so it can't change under us.
    bool is_self = (iter == core_descs->cur_task);
    local_spinlock_unlock(&iter->lock);
    sti(cli_state);

    //A task that put itself to sleep must yield so the core actually switches
    //away; otherwise it keeps running (merely mislabelled task_state_sleep)
    //until the next preemption tick -- and a caller holding cli() never gets
    //one. Sleeping another task only marks it; this core keeps running.
    if (is_self)
        task_yield();
    return CS_OK;
}

cs_error nanosleep_syscall()
{
    return CS_OK;
}

void semaphore_init(semaphore_t *sema)
{
    sema->count = 0;
    sema->spinlock = 0;
}

void semaphore_signal(semaphore_t *sema)
{
    local_spinlock_lock(&sema->spinlock);
    sema->count++;
    local_spinlock_unlock(&sema->spinlock);
}

void semaphore_wait(semaphore_t *sema)
{
    local_spinlock_lock(&sema->spinlock);
    while (sema->count == 0){
        int state = cli();
        task_monitor_noyield(task_current(), (uint32_t*)&sema->count, 0);
        local_spinlock_unlock(&sema->spinlock);
        task_yield();
        sti(state);
        local_spinlock_lock(&sema->spinlock);
    }
    --sema->count;
    local_spinlock_unlock(&sema->spinlock);
}

static void task_ap_entry(void); //defined below; released by task_release_aps()

//Releases the application processors into the scheduler once every Core* server and
//device driver has been loaded (the kernel module loader is single-threaded-only, so
//the APs stay parked in mp_signalready() throughout loading). Each AP then runs
//task_ap_entry: per-core setup (interrupt stack + run queue + idle task), then joins
//scheduling.
//
//With per-core run queues each AP schedules only the tasks on its own queue. Tasks
//created after the APs come online round-robin across cores (see pick_target_core);
//boot-time tasks stay on the BSP. See notes/AUDIT.md.
int task_release_aps()
{
    DEBUG_PRINT("[SysTaskMgr] Boot services loaded; releasing APs into scheduler\r\n");
    mp_set_ap_entry(task_ap_entry);
    return 0;
}

static void NORETURN idle_task(void *arg)
{
    arg = NULL;
    //Always-runnable fallback so a core with no other work never starves the
    //scheduler (which would otherwise PANIC in the scheduler).
    while (true)
        halt();
}

//Per-core scheduler bring-up, run on the BSP and every AP: register this core's
//run queue, allocate its interrupt stack, and create its idle task. Does NOT arm
//the preemption timer -- call task_core_arm() only once every task this core might
//immediately schedule exists, so the first tick cannot fire into an empty run queue.
static void task_core_setup()
{
    //Allocate and setup interrupt stack
    uint8_t *interrupt_stack = (uint8_t *)malloc(KERNEL_STACK_LEN) + KERNEL_STACK_LEN;

    core_descs->interrupt_stack = interrupt_stack;
    core_descs->cur_task = NULL;
    core_descs->prev_dead = NULL;

    //Claim a run-queue slot. The index is sequential (not the sparse APIC id) so
    //it can index run_queues[]/rq_locks[]. The slot's head/lock are statically
    //zero-initialized, so publishing the index (the atomic increment) is enough
    //to make the queue usable by other cores.
    int idx = registered_cores++;
    if (idx >= MAX_CORES)
        PANIC("[SysTaskMgr] Too many cores.");
    core_descs->core_idx = idx;

    interrupt_setstack(interrupt_stack);

    //Each core needs an always-runnable idle task, pinned to this core.
    cs_id idle_id = 0;
    if (create_task_core("idle", task_permissions_kernel, &idle_id, idx) != CS_OK)
        PANIC("[SysTaskMgr] Failed to create idle task.");
    if (task_start_kernel(idle_id, idle_task, NULL) != CS_OK)
        PANIC("[SysTaskMgr] Failed to start idle task.");
}

//Arm this core's periodic preemption timer and enable interrupts. Must be the
//last bring-up step: the first tick abandons the current (boot/AP) context for a
//scheduled task and never returns to it.
static void task_core_arm()
{
    if (timer_request(timer_features_periodic | timer_features_local, 50000, task_switch_handler))
        PANIC("[SysTaskMgr] Failed to allocate periodic timer!");

    __asm__ volatile("sti");
}

//Entry point handed to SysMP via mp_set_ap_entry; each application processor
//runs this once the scheduler is online. It allocates the core's per-core state,
//joins scheduling, then idles until the preemption timer switches it to a task.
static void task_ap_entry(void)
{
    if (core_descs == NULL)
        core_descs = tls_core_descs();
    core_descs->interrupt_stack = NULL;
    core_descs->cur_task = NULL;
    core_descs->prev_dead = NULL;

    task_core_setup();
    task_core_arm();

    while (true)
        halt();
}

void task_startnew_user(void *elf, size_t elf_len)
{
    cs_id elf_id = 0;
    cs_error elf_err = task_create_kernel("elf_test", task_permissions_none, &elf_id);
    if (elf_err != CS_OK)
        PANIC("[SysTaskMgr] Failed to create elf_test task.");

    void (*entry_pt)(void *) = NULL;
    user_elf_load(elf_id, elf, elf_len, &entry_pt);
    elf_err = task_start_kernel(elf_id, entry_pt, NULL);
}

int module_init()
{
    //Allocate core memory
    if (core_descs == NULL)
        core_descs = tls_core_descs();
    core_descs->interrupt_stack = NULL;
    core_descs->cur_task = NULL;
    core_descs->prev_dead = NULL;

    registry_createdirectory("", "procs");

    //Per-core bring-up for the BSP (run queue + interrupt stack + its idle task)
    task_core_setup();

    //K5 (interpreter-as-scheduler): the native preemption scheduler is no longer
    //started here. The boot thread does NOT hand itself off to task_switch_handler;
    //instead module_init RETURNS and the boot script continues into the Lisp
    //runtime (LOAD SysLisp + CALL lisp_scheduler_enter), which becomes the per-core
    //scheduler loop. The native task machinery below (run queues, the idle task,
    //task_core_arm) is dormant during this transition and is removed once nothing
    //native remains. See notes/core/lisp-substrate.md.

    syscall_sethandler(1, (void *)nanosleep_syscall);

    syscall_sethandler(2, (void *)task_map);
    syscall_sethandler(3, (void *)task_updatemap);
    syscall_sethandler(4, (void *)task_unmap);

    syscall_sethandler(5, (void *)task_create_syscall);
    syscall_sethandler(6, (void *)task_start_syscall);
    syscall_sethandler(7, (void *)task_end_syscall);

    syscall_sethandler(8, (void *)openspecialset_syscall); //Request a special set of syscalls to be enabled for this process

    //TODO: consider adding code to SysDebug to allow it to provide support for user mode debuggers

    //NOTE: under K5 the application processors are released into the scheduler by
    //lisp_scheduler_enter (mp_set_ap_entry(lisp_core_loop)), once the Lisp runtime
    //is up -- not here. The kernel module loader is single-threaded-only, so the
    //APs stay parked in mp_signalready() until loading is complete.

    //Return so the boot script continues into the Lisp runtime, which takes over
    //as the per-core scheduler (the native task_core_arm() handoff is gone).
    return 0;
}
