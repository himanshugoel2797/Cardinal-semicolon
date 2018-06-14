/**
 * Copyright (c) 2018 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "SysMP/mp.h"
#include "SysFP/fp.h"
#include "SysPhysicalMemory/phys_mem.h"
#include "SysVirtualMemory/vmem.h"
#include "SysMemory/memory.h"
#include "SysUser/syscall.h"
#include "SysTimer/timer.h"
#include "SysInterrupts/interrupts.h"

#include "task_priv.h"
#include "error.h"
#include "thread.h"
#include <cardinal/local_spinlock.h>

// thread/process id allocator
static _Atomic cs_id cur_id = 1;

// task descriptions
static task_desc_t *tasks = NULL;
static int task_lock = 0;

// process descriptions
static process_desc_t *processes = NULL;
static int process_lock = 0;

// current core description
static TLS core_desc_t *core_descs = NULL;


cs_error create_task_kernel(cs_task_type tasktype, char *name, task_permissions_t perms, cs_id *id) {
    cs_id alloc_id = cur_id++;

    if(tasktype == cs_task_type_process) {
        //Create the process address space and add it to the list
        process_desc_t *proc_info = malloc(sizeof(process_desc_t));
        if(proc_info == NULL) {
            free(proc_info);
            return CS_OUTOFMEM;
        }

        memset(proc_info, 0, sizeof(process_desc_t));
        proc_info->id = alloc_id;
        proc_info->lock = 0;
        proc_info->thd_cnt = 0;

        if(vmem_create(&proc_info->mem) != 0) {
            free(proc_info);
            return CS_OUTOFMEM;
        }

        *id = alloc_id;

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
    }

    if(tasktype == cs_task_type_process || tasktype == cs_task_type_thread) {
        //Create the thread and add it to the list
        task_desc_t *thread_info = malloc(sizeof(task_desc_t));
        if(thread_info == NULL) {
            free(thread_info);
            return CS_OUTOFMEM;
        }

        memset(thread_info, 0, sizeof(task_desc_t));
        memcpy(thread_info->name, name, 256);
        thread_info->state = task_state_uninitialized;
        thread_info->permissions = perms;
        thread_info->signalmask = 0xffffffff;
        thread_info->lock = 0;
        thread_info->pid = (tasktype == cs_task_type_thread) ? core_descs->cur_task->pid : alloc_id;    //TODO: add this to the current process, unless it's an new process
        thread_info->id = alloc_id;

        //Allocate the kernel level stack
        thread_info->kernel_stack = malloc(KERNEL_STACK_LEN);
        if(thread_info->kernel_stack == NULL)
            PANIC("Unexpected memory allocaiton failure.");
        thread_info->kernel_stack += KERNEL_STACK_LEN;

        thread_info->fpu_state = malloc(fp_platform_getstatesize() + fp_platform_getalign());
        if(thread_info->fpu_state == NULL)
            PANIC("Unexpected memory allocation failure.");

        //Align the FPU state properly
        if((uintptr_t)thread_info->fpu_state % fp_platform_getalign() != 0)
            thread_info->fpu_state += fp_platform_getalign() - ((uintptr_t)thread_info->fpu_state % fp_platform_getalign());

        fp_platform_getdefaultstate(thread_info->fpu_state);

        thread_info->reg_state = malloc(mp_platform_getstatesize());
        if(thread_info->reg_state == NULL)
            PANIC("Unexpected memory allocation failure.");
        mp_platform_getdefaultstate(thread_info->reg_state, thread_info->kernel_stack, NULL, NULL);

        //__asm__("cli\n\thlt" :: "a"(thread_info->kernel_stack), "b"(thread_info->fpu_state), "c"(thread_info->reg_state));

        *id = alloc_id;

        //add this to the thread queue
        local_spinlock_lock(&task_lock);

        if(tasks == NULL) {
            thread_info->next = thread_info;
            thread_info->prev = thread_info;

            tasks = thread_info;
        } else {
            local_spinlock_lock(&tasks->lock);

            thread_info->next = tasks->next;
            thread_info->prev = tasks;
            tasks->next->prev = thread_info;
            tasks->next = thread_info;

            local_spinlock_unlock(&tasks->lock);
        }

        local_spinlock_unlock(&task_lock);
    }

    return CS_OK;
}

cs_error start_task_kernel(cs_id id, void(*handler)(void *arg)) {
    local_spinlock_lock(&task_lock);

    task_desc_t *iterator = tasks;
    task_desc_t *task = NULL;
    cs_id first_id = iterator->id;

    //Find the specified task
    do {
        if(iterator->id == id) {
            task = iterator;
            break;
        }
        iterator = iterator->next;
    } while(iterator->id != first_id);
    if(task == NULL)    //Didn't find the id
        return CS_UNKN;

    {
        local_spinlock_lock(&task->lock);
        //TODO: The argument may later be used for initialization parameters for the libc
        mp_platform_getdefaultstate(task->reg_state, task->kernel_stack, handler, NULL);
        task->state = task_state_pending;
        local_spinlock_unlock(&task->lock);
    }
    local_spinlock_unlock(&task_lock);
    return CS_OK;
}

static void task_switch_handler(int irq) {
    irq = 0;

    if(local_spinlock_trylock(&task_lock)) {    //Only switch if the task queue could be locked
        if(local_spinlock_trylock(&process_lock)) {
            if(core_descs->cur_task != NULL) {

                local_spinlock_lock(&core_descs->cur_task->lock);
                core_descs->cur_task->state = task_state_suspended;

                mp_platform_getstate(core_descs->cur_task->reg_state);
                fp_platform_getstate(core_descs->cur_task->fpu_state);

                local_spinlock_unlock(&core_descs->cur_task->lock);
                core_descs->cur_task = core_descs->cur_task->next;
                local_spinlock_lock(&core_descs->cur_task->lock);
                bool loop = true;

                while(loop) {
                    switch(core_descs->cur_task->state) {
                    case task_state_pending:
                    case task_state_suspended:
                    {
                        //resume the task
                        //For user level tasks also find the process and switch vmem table
                        process_desc_t *iterator = processes;
                        do
                            iterator = iterator->next;
                        while(iterator->id != core_descs->cur_task->pid);

                        vmem_setactive(iterator->mem);
                        mp_platform_setstate(core_descs->cur_task->reg_state);
                        fp_platform_setstate(core_descs->cur_task->fpu_state);

                        core_descs->cur_task->state = task_state_running;
                        loop = false;
                    }
                        break;
                    case task_state_running:
                    case task_state_blocked:
                    case task_state_exiting:
                    case task_state_exited:
                    default:
                        local_spinlock_unlock(&core_descs->cur_task->lock);
                        core_descs->cur_task = core_descs->cur_task->next;
                        local_spinlock_lock(&core_descs->cur_task->lock);
                        break;
                    }
                }
                local_spinlock_unlock(&core_descs->cur_task->lock);
            } else {
                core_descs->cur_task = tasks;
                local_spinlock_lock(&core_descs->cur_task->lock);

                process_desc_t *iterator = processes;
                do
                    iterator = iterator->next;
                while(iterator->id != core_descs->cur_task->pid);

                vmem_setactive(iterator->mem);
                fp_platform_setstate(core_descs->cur_task->fpu_state);
                mp_platform_setstate(core_descs->cur_task->reg_state);

                core_descs->cur_task->state = task_state_running;
                local_spinlock_unlock(&core_descs->cur_task->lock);
            }
            local_spinlock_unlock(&process_lock);
        }
        local_spinlock_unlock(&task_lock);
    }
}

cs_error send_ipc_syscall() {
    return CS_OK;
}
cs_error receive_ipc_syscall() {
    return CS_OK;
}

cs_error get_perms_syscall(int flag, uint64_t* result) {
    flag = 0;
    result = NULL;
    return CS_OK;
}
cs_error release_perms_syscall(int flag) {
    flag = 0;
    return CS_OK;
}

cs_error create_task_syscall(cs_task_type tasktype, char *name, cs_id *id) {
    return create_task_kernel(tasktype, name, task_permissions_none, id);
}
cs_error start_task_syscall(cs_id id, void(*handler)(void *arg)) {
    return start_task_kernel(id, handler);
}

cs_error nanosleep_syscall() {
    return CS_OK;
}

cs_error signal_syscall() {
    return CS_OK;
}
cs_error register_signal_syscall() {
    return CS_OK;
}
cs_error signal_mask_syscall() {
    return CS_OK;
}

cs_error exit_syscall() {
    return CS_OK;
}
cs_error kill_task_syscall() {
    return CS_OK;
}

cs_error map_syscall(cs_id pid, intptr_t virt, intptr_t phys, size_t sz, int perms, int flags) {
    pid = 0;
    virt = 0;
    phys = 0;
    sz = 0;
    perms = 0;
    flags = 0;
    return CS_OK;
}
cs_error unmap_syscall(cs_id pid, intptr_t virt, size_t sz) {
    pid = 0;
    virt = 0;
    sz = 0;
    return CS_OK;
}
cs_error read_mmap_syscall(cs_id pid, intptr_t virt, intptr_t *phys, size_t *sz, int *perms, int *flags) {
    pid = 0;
    virt = 0;
    phys = NULL;
    sz = NULL;
    perms = NULL;
    flags = NULL;
    return CS_OK;
}

cs_error pmalloc_syscall(int flags, size_t sz, uintptr_t *addr) {
    //pagealloc_alloc
    *addr = pagealloc_alloc(0, 0, flags, sz);
    if(*addr == (uintptr_t)-1)
        return CS_OUTOFMEM;

    return CS_OK;
}
cs_error pfree_syscall(uintptr_t addr, size_t sz) {
    //pagealloc_free
    pagealloc_free(addr, sz);
    return CS_OK;
}

int servicescript_execute();
void servicescript_handler(void *arg) {
    arg = NULL;

    servicescript_execute();

    PANIC("End of process.");
}


int module_mp_init() {

    //Allocate and setup interrupt stack
    uint8_t* interrupt_stack = (uint8_t*)malloc(KiB(4)) + KiB(4);

    core_descs->interrupt_stack = interrupt_stack;
    core_descs->cur_task = NULL;

    cs_id ss_id = 0;
    cs_error ss_err = create_task_kernel(cs_task_type_process, "servicescript", task_permissions_kernel, &ss_id);
    if(ss_err != CS_OK)
        PANIC("SS_ERR0");
    ss_err = start_task_kernel(ss_id, servicescript_handler);
    if(ss_err != CS_OK)
        PANIC("SS_ERR1");

    interrupt_setstack(interrupt_stack);

    if(timer_request(timer_features_periodic | timer_features_local, 50000, task_switch_handler))
        PANIC("Failed to allocate periodic timer!");

    return 0;
}

int module_init() {

    //Allocate core memory
    if(core_descs == NULL)
        core_descs = (TLS core_desc_t*)mp_tls_get(mp_tls_alloc(sizeof(core_desc_t)));

    module_mp_init();

    //Enable server load tasks
    //Server load tasks work by creating a new task with the elf loader and passing in the program tar for extraction and loading


    //Install ipc, get_perms, release_perms, tasking syscalls
    syscall_sethandler(0, (void*)send_ipc_syscall);
    syscall_sethandler(1, (void*)receive_ipc_syscall);

    syscall_sethandler(10, (void*)get_perms_syscall);
    syscall_sethandler(11, (void*)release_perms_syscall);

    syscall_sethandler(20, (void*)create_task_syscall);
    syscall_sethandler(23, (void*)start_task_syscall);

    syscall_sethandler(25, (void*)nanosleep_syscall);

    syscall_sethandler(26, (void*)signal_syscall);
    syscall_sethandler(27, (void*)register_signal_syscall);
    syscall_sethandler(28, (void*)signal_mask_syscall);

    syscall_sethandler(29, (void*)exit_syscall);
    syscall_sethandler(30, (void*)kill_task_syscall);

    //add map, unmap and pmalloc and pfree syscalls
    syscall_sethandler(31, (void*)map_syscall);
    syscall_sethandler(32, (void*)unmap_syscall);
    syscall_sethandler(33, (void*)read_mmap_syscall);

    syscall_sethandler(34, (void*)pmalloc_syscall);
    syscall_sethandler(35, (void*)pfree_syscall);

    //TODO: consider adding code to SysDebug to allow it to provide support for user mode debuggers

    //Make sure that execution on the boot path doesn't continue past here.
    while(true)
        halt();

    return 0;
}