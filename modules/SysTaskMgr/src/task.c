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

cs_error create_task_kernel(char *name, task_permissions_t perms, cs_id *id) {
    cs_id alloc_id = cur_id++;

    //Create the process address space and add it to the list
    process_desc_t *proc_info = malloc(sizeof(process_desc_t));
    DEBUG_PRINT("Process Created: ");
    DEBUG_PRINT(name);
    DEBUG_PRINT("\r\n");
    if(proc_info == NULL) {
        free(proc_info);
        return CS_OUTOFMEM;
    }

    memset(proc_info, 0, sizeof(process_desc_t));
    strncpy(proc_info->name, name, 256);
    proc_info->id = alloc_id;
    proc_info->lock = 0;

    if(vmem_create(&proc_info->mem) != 0) {
        free(proc_info);
        return CS_OUTOFMEM;
    }

    *id = alloc_id;

    proc_info->state = task_state_uninitialized;
    proc_info->permissions = perms;

    //Allocate the kernel level stack
    proc_info->kernel_stack = malloc(KERNEL_STACK_LEN);
    if(proc_info->kernel_stack == NULL)
        PANIC("Unexpected memory allocaiton failure.");
    proc_info->kernel_stack += KERNEL_STACK_LEN;

    proc_info->fpu_state = malloc(fp_platform_getstatesize() + fp_platform_getalign());
    if(proc_info->fpu_state == NULL)
        PANIC("Unexpected memory allocation failure.");

    //Align the FPU state properly
    if((uintptr_t)proc_info->fpu_state % fp_platform_getalign() != 0)
        proc_info->fpu_state += fp_platform_getalign() - ((uintptr_t)proc_info->fpu_state % fp_platform_getalign());

    fp_platform_getdefaultstate(proc_info->fpu_state);

    proc_info->reg_state = malloc(mp_platform_getstatesize());
    if(proc_info->reg_state == NULL)
        PANIC("Unexpected memory allocation failure.");
    mp_platform_getdefaultstate(proc_info->reg_state, proc_info->kernel_stack, NULL, NULL);


    //add this to the process queue
    local_spinlock_lock(&process_lock);

    if(processes == NULL) {
        proc_info->prev = proc_info;
        proc_info->next = proc_info;
        processes = proc_info;
    } else {
        local_spinlock_lock(&processes->lock);

        proc_info->next = processes->next;
        proc_info->prev = processes;
        processes->next->prev = proc_info;
        processes->next = proc_info;

        local_spinlock_unlock(&processes->lock);

        processes = proc_info;
    }
    local_spinlock_unlock(&process_lock);
    process_count++;

    return CS_OK;
}

cs_error nanosleep_syscall() {
    return CS_OK;
}

int servicescript_execute();
void servicescript_handler(void *arg) {
    arg = NULL;

    servicescript_execute();

    while(true) halt(); //TODO: Implemented process termination
}

int module_mp_init() {

    //Allocate and setup interrupt stack
    uint8_t* interrupt_stack = (uint8_t*)malloc(KERNEL_STACK_LEN) + KERNEL_STACK_LEN;

    core_descs->interrupt_stack = interrupt_stack;
    core_descs->cur_task = NULL;

    interrupt_setstack(interrupt_stack);

    cs_id ss_id = 0;
    cs_error ss_err = create_task_kernel("servicescript", task_permissions_kernel, &ss_id);
    if(ss_err != CS_OK)
        PANIC("SS_ERR0");
    /*ss_err = start_task_kernel(ss_id, servicescript_handler);
    if(ss_err != CS_OK)
        PANIC("SS_ERR1");

    if(timer_request(timer_features_periodic | timer_features_local, 50000, task_switch_handler))
        PANIC("Failed to allocate periodic timer!");
    */
    return 0;
}

int module_init() {

    //Allocate core memory
    if(core_descs == NULL)
        core_descs = (TLS core_desc_t*)mp_tls_get(mp_tls_alloc(sizeof(core_desc_t)));

    registry_createdirectory("", "procs");
    module_mp_init();

    //Enable server load tasks
    //Server load tasks work by creating a new task with the elf loader and passing in the program tar for extraction and loading
    //syscall_sethandler(0, (void*)create_pipe_syscall);

    //syscall_sethandler(30, (void*)nanosleep_syscall);

    //TODO: consider adding code to SysDebug to allow it to provide support for user mode debuggers

    //Make sure that execution on the boot path doesn't continue past here.
    while(true)
        halt();

    return 0;
}