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
        proc_info->prev = proc_info;
        proc_info->next = proc_info;
        processes = proc_info;
    }
    else
    {
        local_spinlock_lock(&processes->lock);

        proc_info->next = processes->next;
        proc_info->prev = processes;
        processes->next->prev = proc_info;
        processes->next = proc_info;

        local_spinlock_unlock(&processes->lock);

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

static void task_switch_handler(int irq)
{
    irq = 0;

    int cli_state = cli();
    local_spinlock_lock(&process_lock);

    process_desc_t *ntask = NULL; //find the first pending task
    if (core_descs->cur_task != NULL)
    {
        local_spinlock_lock(&core_descs->cur_task->lock);

        core_descs->cur_task->state = task_state_pending;      //Set the cur_task to pending again
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

cs_error nanosleep_syscall()
{
    return CS_OK;
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

    cs_id ss_id = 0;
    cs_error ss_err = create_task_kernel("servicescript", task_permissions_kernel, &ss_id);
    if (ss_err != CS_OK)
        PANIC("[SysTaskMgr] SS_ERR0");
    ss_err = start_task_kernel(ss_id, servicescript_handler, NULL);
    if (ss_err != CS_OK)
        PANIC("[SysTaskMgr] SS_ERR1");

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

    //Enable server load tasks
    //Server load tasks work by creating a new task with the elf loader and passing in the program tar for extraction and loading
    //syscall_sethandler(0, (void*)create_pipe_syscall);

    //syscall_sethandler(30, (void*)nanosleep_syscall);

    //TODO: consider adding code to SysDebug to allow it to provide support for user mode debuggers

    //Make sure that execution on the boot path doesn't continue past here.
    while (true)
        halt();

    return 0;
}