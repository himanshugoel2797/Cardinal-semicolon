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

#include "task_priv.h"
#include "error.h"
#include "thread.h"

// thread/process id allocator
static _Atomic cs_id cur_id = 1;

// process descriptions
static process_desc_t *processes = NULL;
static _Atomic int process_count = 0;
static int process_lock = 0;

// current core description
static TLS core_desc_t *core_descs = NULL;

cs_error create_task_kernel(char *name, task_permissions_t perms, cs_id *id)
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
        PANIC("[SysTaskMgr] Unexpected memory allocaiton failure.");
    proc_info->kernel_stack += KERNEL_STACK_LEN;

    proc_info->fpu_state = malloc(fp_platform_getstatesize() + fp_platform_getalign());
    if (proc_info->fpu_state == NULL)
        PANIC("[SysTaskMgr] Unexpected memory allocation failure.");

    //Align the FPU state properly
    if ((uintptr_t)proc_info->fpu_state % fp_platform_getalign() != 0)
        proc_info->fpu_state += fp_platform_getalign() - ((uintptr_t)proc_info->fpu_state % fp_platform_getalign());

    fp_platform_getdefaultstate(proc_info->fpu_state);

    proc_info->reg_state = malloc(mp_platform_getstatesize());
    if (proc_info->reg_state == NULL)
        PANIC("[SysTaskMgr] Unexpected memory allocation failure.");
    mp_platform_getdefaultstate(proc_info->reg_state, proc_info->kernel_stack, NULL, NULL);

    //add this to the process queue
    int cli_state = cli();
    local_spinlock_lock(&process_lock);
    if (processes == NULL)
    {
        proc_info->next = NULL;
        processes = proc_info;
    }
    else
    {
        proc_info->next = processes;
        processes = proc_info;
    }
    local_spinlock_unlock(&process_lock);
    sti(cli_state);

    process_count++;

    return CS_OK;
}

cs_error start_task_kernel(cs_id id, void (*handler)(void *arg), void *arg)
{
    if (handler != NULL)
    {
        int cli_state = cli();
        local_spinlock_lock(&process_lock);
        if (processes != NULL)
        {
            process_desc_t *iter = processes;
            while (iter != NULL)
            {
                process_desc_t *cur_iter = iter;
                local_spinlock_lock(&cur_iter->lock);
                if (iter->id == id)
                    break;
                iter = iter->next;
                local_spinlock_unlock(&cur_iter->lock);
            }
            if (iter != NULL)
            {
                //Lock is already held from the break in the previous loop
                //Entry found
                DEBUG_PRINT("[SysTaskMgr] Process Started: ");
                DEBUG_PRINT(iter->name);
                DEBUG_PRINT("\r\n");

                mp_platform_getdefaultstate(iter->reg_state, iter->kernel_stack, (void *)handler, arg); //Rebuild stack state
                iter->state = task_state_pending;                                                       //Set task to initialized

                local_spinlock_unlock(&iter->lock);
            }
            local_spinlock_unlock(&process_lock);
            sti(cli_state);
            return CS_OK;
        }
        local_spinlock_unlock(&process_lock);
        sti(cli_state);
    }
    return CS_UNKN;
}

cs_error create_task_syscall(char *name, cs_id *id)
{
    return create_task_kernel(name, task_permissions_none, id);
}

cs_error start_task_syscall(cs_id id, void (*handler)(void *arg), void *arg)
{
    return start_task_kernel(id, handler, arg);
}

static cs_id alloc_descriptor(process_desc_t *pinfo)
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
            //Restart iteration along sub table
            i = -1;
            d = d[i].desc_entry;
            id += i;
            continue;
        }
        if (d[i].type == descriptor_type_unused_entry)
        {
            //Valid entry found
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
            //Restart iteration along sub table
            i = -1;
            d = d[i].desc_entry;
            base_id += i;
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

static void task_switch_handler(int irq)
{
    irq = 0;

    int cli_state = cli();
    local_spinlock_lock(&process_lock);

    process_desc_t *ntask = NULL; //find the first pending task
    if (core_descs->cur_task != NULL)
    {
        local_spinlock_lock(&core_descs->cur_task->lock);

        if (core_descs->cur_task->state == task_state_running)
            core_descs->cur_task->state = task_state_pending;  //Set the cur_task to pending again
        fp_platform_getstate(core_descs->cur_task->fpu_state); //Save the current tasks's fpu state
        mp_platform_getstate(core_descs->cur_task->reg_state); //Save the current task's register state

        //find the next pending task
        ntask = core_descs->cur_task->next;
        local_spinlock_unlock(&core_descs->cur_task->lock);

        while (ntask != NULL)
        {
            process_desc_t *cur_ntask = ntask;
            local_spinlock_lock(&cur_ntask->lock);
            if (ntask->state == task_state_pending)
            {
                local_spinlock_unlock(&cur_ntask->lock);
                break;
            }
            ntask = ntask->next;
            local_spinlock_unlock(&cur_ntask->lock);
        }
    }

    //if an appropriate task could not be found, iterate over the entire list to find a task
    if (ntask == NULL)
    {
        ntask = processes;
        while (ntask != NULL)
        {
            process_desc_t *cur_ntask = ntask;
            local_spinlock_lock(&cur_ntask->lock);
            if (ntask->state == task_state_pending)
            {
                local_spinlock_unlock(&cur_ntask->lock);
                break;
            }
            ntask = ntask->next;
            local_spinlock_unlock(&cur_ntask->lock);
        }
    }

    //if an appropriate task could still not be found, panic
    if (ntask == NULL)
    {
        PANIC("[SysTaskMgr] Out of Processes!\r\n");
    }

    //switch to this task
    core_descs->cur_task = ntask;

    local_spinlock_lock(&ntask->lock);
    vmem_setactive(ntask->mem);             //Set virtual memory
    fp_platform_setstate(ntask->fpu_state); //Set fpu state
    mp_platform_setstate(ntask->reg_state); //Set registers
    local_spinlock_unlock(&ntask->lock);

    local_spinlock_unlock(&process_lock);
    sti(cli_state);
}

cs_error task_map(cs_id id, const char *name, intptr_t vaddr, size_t sz, task_map_flags_t flags, task_map_perms_t owner_perms, task_map_perms_t child_perms, int child_count, cs_id *shmem_id)
{
    name = NULL;
    if (shmem_id == NULL)
        return CS_UNKN;

    if (name == NULL && (flags & task_map_shared) != 0)
        return CS_UNKN;

    if ((flags & task_map_oneway) != 0 && (flags & task_map_shared) == 0) //oneway mapping is only valid with shared memory
        return CS_UNKN;

    if ((flags & task_map_oneuse) != 0 && (flags & task_map_shared) == 0) //oneuse mapping is only valid with shared memory
        return CS_UNKN;

    if ((flags & task_map_shared) != 0)
        PANIC("[SysTaskMgr] Shared memory not implemented.");

    int cli_state = cli();
    local_spinlock_lock(&process_lock);
    if (processes != NULL)
    {
        process_desc_t *iter = processes;
        while (iter != NULL)
        {
            process_desc_t *cur_iter = iter;
            local_spinlock_lock(&cur_iter->lock);
            if (iter->id == id)
                break;
            iter = iter->next;
            local_spinlock_unlock(&cur_iter->lock);
        }
        if (iter != NULL)
        {
            //Lock is already held from the break in the previous loop
            cs_id shmem_k_id = alloc_descriptor(iter);
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
                uintptr_t pmem = pagealloc_alloc(0, 0, physmem_alloc_flags_data | physmem_alloc_flags_instr | physmem_alloc_flags_zero, sz);
                d->map_entry->paddr = pmem;
                vmem_map(iter->mem, d->map_entry->vaddr, (intptr_t)pmem, sz, map_perms, 0);
            }

            *shmem_id = shmem_k_id;
            local_spinlock_unlock(&iter->lock);
        }
        local_spinlock_unlock(&process_lock);
        sti(cli_state);
        return CS_OK;
    }
    local_spinlock_unlock(&process_lock);
    sti(cli_state);
    return CS_UNKN;
}

cs_error task_updatemap(cs_id id, cs_id shmem_id, task_map_perms_t perms)
{
    int cli_state = cli();
    local_spinlock_lock(&process_lock);
    if (processes != NULL)
    {
        process_desc_t *iter = processes;
        while (iter != NULL)
        {
            process_desc_t *cur_iter = iter;
            local_spinlock_lock(&cur_iter->lock);
            if (iter->id == id)
                break;
            iter = iter->next;
            local_spinlock_unlock(&cur_iter->lock);
        }
        if (iter != NULL)
        {
            //Lock is already held from the break in the previous loop
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

                vmem_unmap(iter->mem, d->map_entry->vaddr, d->map_entry->sz);
                vmem_map(iter->mem, d->map_entry->vaddr, (intptr_t)d->map_entry->paddr, d->map_entry->sz, map_perms, 0);
            }
            local_spinlock_unlock(&iter->lock);
        }
        local_spinlock_unlock(&process_lock);
        sti(cli_state);
        return CS_OK;
    }
    local_spinlock_unlock(&process_lock);
    sti(cli_state);
    return CS_UNKN;
}

cs_error task_unmap(cs_id id, cs_id shmem_id)
{
    int cli_state = cli();
    local_spinlock_lock(&process_lock);
    if (processes != NULL)
    {
        process_desc_t *iter = processes;
        while (iter != NULL)
        {
            process_desc_t *cur_iter = iter;
            local_spinlock_lock(&cur_iter->lock);
            if (iter->id == id)
                break;
            iter = iter->next;
            local_spinlock_unlock(&cur_iter->lock);
        }
        if (iter != NULL)
        {
            //Lock is already held from the break in the previous loop
            descriptor_entry_t *d = read_descriptor(iter, shmem_id);
            if (d->type == descriptor_type_map_entry)
            {
                //Unmap memory region
                vmem_unmap(iter->mem, d->map_entry->vaddr, d->map_entry->sz);
                if (d->map_entry->is_owner)
                {
                    //free physical memory
                    pagealloc_free(d->map_entry->paddr, d->map_entry->sz);
                }

                free(d->map_entry);
                d->map_entry = NULL;
                d->type = descriptor_type_unused_entry;
            }
            local_spinlock_unlock(&iter->lock);
        }
        local_spinlock_unlock(&process_lock);
        sti(cli_state);
        return CS_OK;
    }
    local_spinlock_unlock(&process_lock);
    sti(cli_state);
    return CS_UNKN;
}

cs_error nanosleep_syscall()
{
    return CS_OK;
}

static void task_cleanup(void *arg)
{
    arg = NULL;

    while (true)
    {
        //Delete dead threads
        int cli_state = cli();
        local_spinlock_lock(&process_lock);
        process_desc_t *iter = processes;
        process_desc_t *prev_iter = NULL;

        while (iter != NULL)
        {
            process_desc_t *cur_iter = iter;
            if (!local_spinlock_trylock(&cur_iter->lock))
                break;
            if (iter->state == task_state_exited)
            {
                //Delete task
                if (iter->mem != NULL)
                {
                    free_descriptors(iter, iter->descriptors, 0); //Unmap/free all descriptors regions
                    vmem_destroy(iter->mem);
                    free(iter->fpu_state);
                    free(iter->reg_state);
                    free(iter->kernel_stack);
                    iter->mem = NULL;
                }

                if (prev_iter != NULL)
                {
                    processes = iter->next;
                    free(iter);
                    process_count--;
                }
                else if (local_spinlock_trylock(&prev_iter->lock))
                {
                    prev_iter->next = iter->next;
                    local_spinlock_unlock(&prev_iter->lock);

                    free(iter);
                    process_count--; //NOTE: This will probably leak if the prev_iter is also task_state_exited
                }
                else
                    local_spinlock_unlock(&cur_iter->lock);
                break; //Exit inner loop every free to allow pre-emption
            }
            prev_iter = iter;
            iter = iter->next;
            local_spinlock_unlock(&cur_iter->lock);
        }

        local_spinlock_unlock(&process_lock);
        sti(cli_state);
    }
}

int servicescript_execute();
void servicescript_handler(void *arg)
{
    arg = NULL;

    servicescript_execute();

    while (true)
        halt(); //TODO: Implement process termination
}

int module_mp_init()
{
    //Allocate and setup interrupt stack
    uint8_t *interrupt_stack = (uint8_t *)malloc(KERNEL_STACK_LEN) + KERNEL_STACK_LEN;

    core_descs->interrupt_stack = interrupt_stack;
    core_descs->cur_task = NULL;

    interrupt_setstack(interrupt_stack);

    //Launch servicescript task
    cs_id ss_id = 0;
    cs_error ss_err = create_task_kernel("servicescript", task_permissions_kernel, &ss_id);
    if (ss_err != CS_OK)
        PANIC("[SysTaskMgr] Failed to create servicescript task.");
    ss_err = start_task_kernel(ss_id, servicescript_handler, NULL);
    if (ss_err != CS_OK)
        PANIC("[SysTaskMgr] Failed to start servicescript task.");

    //Launch task cleanup task
    cs_id tc_id = 0;
    cs_error tc_err = create_task_kernel("task_cleanup", task_permissions_kernel, &tc_id);
    if (tc_err != CS_OK)
        PANIC("[SysTaskMgr] Failed to create task_cleanup task.");
    tc_err = start_task_kernel(tc_id, task_cleanup, NULL);
    if (tc_err != CS_OK)
        PANIC("[SysTaskMgr] Failed to start task_cleanup task.");

    if (timer_request(timer_features_periodic | timer_features_local, 50000, task_switch_handler))
        PANIC("[SysTaskMgr] Failed to allocate periodic timer!");
    return 0;
}

int module_init()
{
    //Allocate core memory
    if (core_descs == NULL)
        core_descs = (TLS core_desc_t *)mp_tls_get(mp_tls_alloc(sizeof(core_desc_t)));
    core_descs->interrupt_stack = NULL;
    core_descs->cur_task = NULL;

    registry_createdirectory("", "procs");
    module_mp_init();

    syscall_sethandler(1, (void *)nanosleep_syscall);

    syscall_sethandler(2, (void *)task_map);
    syscall_sethandler(3, (void *)task_updatemap);
    syscall_sethandler(4, (void *)task_unmap);

    syscall_sethandler(5, (void *)create_task_syscall);
    syscall_sethandler(6, (void *)start_task_syscall);

    //TODO: consider adding code to SysDebug to allow it to provide support for user mode debuggers

    //Make sure that execution on the boot path doesn't continue past here.
    while (true)
        halt();

    task_cleanup(NULL);

    return 0;
}