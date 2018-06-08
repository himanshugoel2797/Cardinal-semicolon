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
#include "SysMemory/memory.h"
#include "SysUser/syscall.h"
#include "task_priv.h"
#include "error.h"
#include "thread.h"

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


int module_mp_init(){
    
    //Allocate kernel stack and setup interrupt stack

    return 0;
}

int module_init(){

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
    syscall_sethandler(21, (void*)write_process_memory_syscall);
    syscall_sethandler(22, (void*)read_process_memory_syscall);
    syscall_sethandler(23, (void*)start_task_syscall);

    syscall_sethandler(25, (void*)nanosleep_syscall);
    
    syscall_sethandler(26, (void*)signal_syscall);
    syscall_sethandler(27, (void*)register_signal_syscall);
    syscall_sethandler(28, (void*)signal_mask_syscall);
    
    syscall_sethandler(29, (void*)exit_syscall);
    syscall_sethandler(30, (void*)kill_task_syscall);

    return 0;
}

cs_error create_task_kernel(cs_task_type tasktype, char *name, task_permissions_t perms, cs_id *id){
    cs_id alloc_id = cur_id++;

    if(tasktype == cs_task_type_process)
    {
        //Create the process address space and add it to the list
        process_desc_t *proc_info = malloc(sizeof(process_desc_t));
        if(proc_info == NULL)
            {
                free(proc_info);
                return CS_OUTOFMEM;
            }

        memset(proc_info, 0, sizeof(process_desc_t));
        proc_info->id = alloc_id;
        proc_info->lock = 0;
        proc_info->thd_cnt = 0;

        if(vmem_create(&proc_info->mem) != 0)
            {
                free(proc_info);
                return CS_OUTOFMEM;
            }

        *id = alloc_id;

        //TODO: add this to the process queue
    }

    if(tasktype == cs_task_type_process || tasktype == cs_task_type_thread)
    {
        //Create the thread and add it to the list
        task_desc_t *thread_info = malloc(sizeof(task_desc_t));
        if(thread_info == NULL)
        {
            free(thread_info);
            return CS_OUTOFMEM;
        }

        memset(thread_info, 0, sieof(task_desc_t));
        memcpy(thread_info->name, name, 256);
        thread_info->state = task_state_pending;
        thread_info->permissions = perms;
        thread_info->signalmask = 0xffffffff;
        thread_info->lock = 0;
        thread_info->pid = alloc_id;
        thread_info->id = alloc_id;


        *id = alloc_id;

        //TODO: add this to the thread queue
    }

    return CS_OK;
}


cs_error send_ipc_syscall(){
    return CS_OK;
}
cs_error receive_ipc_syscall(){
    return CS_OK;
}

cs_error get_perms_syscall(int flag, uint64_t* result){
    return CS_OK;
}
cs_error release_perms_syscall(int flag){
    return CS_OK;
}

cs_error create_task_syscall(cs_task_type tasktype, char *name, cs_id *id){
    return create_task_kernel(tasktype, name, task_permissions_none, id);
}
cs_error write_process_memory_syscall(){
    return CS_OK;
}
cs_error read_process_memory_syscall(){
    return CS_OK;
}
cs_error start_task_syscall(){
    return CS_OK;
}


cs_error nanosleep_syscall(){
    return CS_OK;
}

cs_error signal_syscall(){
    return CS_OK;
}
cs_error register_signal_syscall(){
    return CS_OK;
}
cs_error signal_mask_syscall(){
    return CS_OK;
}

cs_error exit_syscall(){
    return CS_OK;
}
cs_error kill_task_syscall(){
    return CS_OK;
}