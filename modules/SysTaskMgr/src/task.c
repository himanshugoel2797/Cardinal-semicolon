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

// task descriptions
static task_desc_t *tasks = NULL;
static int task_lock = 0;

// process descriptions
static process_desc_t *processes = NULL;
static _Atomic int process_count = 0;
static int process_lock = 0;

// current core description
static TLS core_desc_t *core_descs = NULL;

#define PIPE_BASE_ADDR (0x018000000000)

cs_error create_task_kernel(cs_task_type tasktype, char *name, task_permissions_t perms, cs_id *id) {
    cs_id alloc_id = cur_id++;

    if(tasktype == cs_task_type_process) {
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
        proc_info->thd_cnt = 0;
        proc_info->desc_id = 0;
        proc_info->pipe_base_vmem = (intptr_t)PIPE_BASE_ADDR;

        if(vmem_create(&proc_info->mem) != 0) {
            list_fini(&proc_info->owned_caps);
            list_fini(&proc_info->pipes);
            free(proc_info);
            return CS_OUTOFMEM;
        }

        char name_buf[TASK_NAME_LEN + 1];
        char dirpath_buf[TASK_NAME_LEN + TASK_NAME_LEN + 2] = "procs/";
        memset(name_buf, 0, TASK_NAME_LEN + 1);
        strncpy(name_buf, name, TASK_NAME_LEN);
        strncat(dirpath_buf, name_buf, TASK_NAME_LEN);
        registry_createdirectory("procs", name_buf);
        registry_createdirectory(dirpath_buf, "pipes");
        registry_createdirectory(dirpath_buf, "capabilities");
        registry_addkey_uint(dirpath_buf, "id", proc_info->id);

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
        process_count++;

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
        thread_info->lock = 0;
        thread_info->pid = (tasktype == cs_task_type_thread) ? core_descs->cur_task->pid : alloc_id;    //add this to the current process, unless it's an new process
        thread_info->id = alloc_id;
        thread_info->sleep_starttime = 0;

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
                    case task_state_suspended: {
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

cs_error create_pipe_syscall(const char *name, const char *capability_name, uint32_t sz, pipe_flags_t flags){
    char path[TASK_NAME_LEN + 1 + 6] = "procs/";
    
    char name_buf[TASK_NAME_LEN + 1];
    strncpy(name_buf, name, TASK_NAME_LEN);

    local_spinlock_lock(&process_lock);
    flags &= ~pipe_flags_nocap;
    process_desc_t *iterator = processes;
    do
        iterator = iterator->next;
    while(iterator->id != core_descs->cur_task->pid);

    //Ensure that the process has the capabilities it claims to require
    if(capability_name != NULL){
        bool capability_matched = false;
        for(uint64_t i = 0; i < list_len(&iterator->owned_caps); i++){
            owned_cap_t* cap = (owned_cap_t*)list_at(&iterator->owned_caps, i);
            if(strncmp(cap->name, capability_name, TASK_NAME_LEN)){
                capability_matched = true;
                break;
            }
        }
        if(!capability_matched){
            local_spinlock_unlock(&process_lock);
            return CS_UNKN; //The capability_name does not exist
        }
    }

    pipe_info_t *pipe_info = malloc(sizeof(pipe_info_t));
    pipe_info->id = cur_id++;
    pipe_info->owner_process_id = core_descs->cur_task->pid;
    pipe_info->user_process_id = 0;
    pipe_info->flags |= flags;
    pipe_info->sz = sz;
    pipe_info->pages = NULL;
    strncpy(pipe_info->name, name_buf, TASK_NAME_LEN);
    if(capability_name != NULL)strncpy(pipe_info->cap_name, capability_name, TASK_NAME_LEN);
    else pipe_info->flags |= pipe_flags_nocap;

    strncat(path, iterator->name, TASK_NAME_LEN);
    registry_addkey_uint(path, name_buf, (uint64_t)pipe_info);

    local_spinlock_unlock(&process_lock);
    return CS_OK;
}

#define ROUNDUP(x, y) (((x - 1) | (y - 1)) + 1)
static void delete_pipe(pipe_info_t *pipe, const char *path, const char *keyname){
    registry_removekey(path, keyname);
    if(pipe->pages != NULL){
        int page_cnt = ROUNDUP(pipe->sz, 4096) / 4096;
        for(int i = 0; i < page_cnt; i++)
            pagealloc_free(pipe->pages[i], KiB(4));
    }
    free(pipe->pages);
    free(pipe);
}

cs_error open_pipe_syscall(const char *name, intptr_t *mem, pipe_t *pipe_id){
    //Allocate memory for the pipe, map it into the current process's memory
    local_spinlock_lock(&process_lock);
    process_desc_t *iterator = processes;
    do
        iterator = iterator->next;
    while(iterator->id != core_descs->cur_task->pid);

    char path[2 * TASK_NAME_LEN + 2] = "";
    char proc_name[TASK_NAME_LEN + 1] = "";
    char keyname[TASK_NAME_LEN + 1] = "";
    memset(path, 0, 2 * TASK_NAME_LEN + 2);
    memset(keyname, 0, TASK_NAME_LEN + 1);

    int path_off = 0, keyname_off = 0, proc_name_off = 0;
    int second_slash_off = -1;
    int slash_cnt = 0;
    for(int i = 0; i < TASK_NAME_LEN * 3 + 2 && name[i] != 0; i++){
        if(slash_cnt < 2){
            path[path_off++] = name[i];
            path[path_off] = 0;
        }
        if(slash_cnt == 1){
            proc_name[proc_name_off++] = name[i];
            proc_name[proc_name_off] = 0;
        }
        if(slash_cnt == 2){
            keyname[keyname_off++] = name[i];
            keyname[keyname_off] = 0;
        }

        if(name[i] == '/'){
            slash_cnt++;
            if(slash_cnt == 2){
                second_slash_off = i;
                break;
            }
        }
    }

    pipe_info_t *pipe_info = NULL;
    if(registry_readkey_uint(path, keyname, (uint64_t*)&pipe_info) != registry_err_ok){
        local_spinlock_unlock(&process_lock);
        return CS_UNKN;
    }

    int page_cnt = ROUNDUP(pipe_info->sz, 4096) / 4096;
    if(pipe_info->pages == NULL){
        pipe_info->pages = malloc(page_cnt * sizeof(uint64_t));
        for(int i = 0; i < page_cnt; i++)
            pipe_info->pages[i] = pagealloc_alloc(0, 0, physmem_alloc_flags_data, KiB(4));
    }

    //create a descriptor
    pipe_descriptor_t *desc = malloc(sizeof(pipe_descriptor_t));
    strncpy(desc->proc_name, proc_name, TASK_NAME_LEN);
    strncpy(desc->pipe_name, keyname, TASK_NAME_LEN);
    desc->descriptor = iterator->desc_id++;
    desc->addr = iterator->pipe_base_vmem;

    if(pipe_info->owner_process_id == core_descs->cur_task->pid){
        //open the pipe as the owner
        //map in the allocated memory
        intptr_t ptr_base = desc->addr;
        for(int i = 0; i < page_cnt; i++, ptr_base += KiB(4))
            vmem_map(iterator->mem, ptr_base, (intptr_t)pipe_info->pages[i], KiB(4), vmem_flags_write | vmem_flags_read | vmem_flags_user | vmem_flags_cachewritethrough, 0);

        iterator->pipe_base_vmem += page_cnt * KiB(4);
        *pipe_id = pipe_info->id;
        *mem = desc->addr;
        list_append(&iterator->pipes, desc);
    }else if(pipe_info->user_process_id == 0){
        //check if the pipe requires any capabilities
        //if so, verify that the current process has that capability
        if((pipe_info->flags & pipe_flags_nocap) == 0){
            bool has_perms = false;
            process_desc_t *parent = processes;
            do
                parent = parent->next;
            while(parent->id != pipe_info->owner_process_id && parent->next != processes);
            for(uint64_t i = 0; i < list_len(&parent->owned_caps) && !has_perms; i++){
                owned_cap_t* cap = (owned_cap_t*)list_at(&parent->owned_caps, i);
                if(strncmp(cap->name, pipe_info->cap_name, TASK_NAME_LEN) == 0)
                    for(uint64_t j = 0; j < list_len(&cap->shared_processes); j++){
                        cs_id id = (cs_id)list_at(&cap->shared_processes, j);
                        if(id == core_descs->cur_task->pid){
                            has_perms = true;
                            break;
                        }
                    }
            }
            if(!has_perms){
                local_spinlock_unlock(&process_lock);
                return CS_UNKN; //the process does not have the necessary permissions
            }
        }

        //open the pipe as the user
        pipe_info->user_process_id = core_descs->cur_task->pid;

        //map in the allocated memory
        intptr_t ptr_base = desc->addr;
        for(int i = 0; i < page_cnt; i++, ptr_base += KiB(4))
            vmem_map(iterator->mem, ptr_base, (intptr_t)pipe_info->pages[i], KiB(4), vmem_flags_write | vmem_flags_read | vmem_flags_user | vmem_flags_cachewritethrough, 0);
        
        iterator->pipe_base_vmem += page_cnt * KiB(4);
        *pipe_id = pipe_info->id;
        *mem = desc->addr;
        list_append(&iterator->pipes, desc);
    }else if(pipe_info->owner_process_id == 0){
        //fail and remove the pipe entry
        local_spinlock_unlock(&process_lock);
        return CS_UNKN; //pipe has been deleted (does not exist anymore)
    }else{
        local_spinlock_unlock(&process_lock);
        return CS_UNKN; //pipe is busy
    }

    local_spinlock_unlock(&process_lock);
    return CS_OK;
}

cs_error close_pipe_syscall(pipe_t pipe_id){
    //Unmap the pipe from the current process's memory and remove it from the descriptor list
    local_spinlock_lock(&process_lock);
    process_desc_t *iterator = processes;
    do
        iterator = iterator->next;
    while(iterator->id != core_descs->cur_task->pid);

    for(uint64_t i = 0; i < list_len(&iterator->pipes); i++){
        pipe_descriptor_t* desc = (pipe_descriptor_t*)list_at(&iterator->pipes, i);
        if(desc->descriptor == pipe_id){
            //Get the pipe info
            char path[TASK_NAME_LEN + 7] = "procs/";
            strncat(path, desc->proc_name, TASK_NAME_LEN);

            char pipe_name[TASK_NAME_LEN + 1] = "";
            strncat(pipe_name, desc->pipe_name, TASK_NAME_LEN);

            pipe_info_t* pipe_info = NULL;
            if(registry_readkey_uint(path, pipe_name, (uint64_t*)&pipe_info) != registry_err_ok){
                local_spinlock_unlock(&process_lock);
                return CS_UNKN; //pipe does not exist
            }

            //Unmap the memory region
            int page_cnt = ROUNDUP(pipe_info->sz, 4096) / 4096;
            intptr_t vaddr = desc->addr;
            for(int j = 0; j < page_cnt; j++, vaddr += KiB(4))
                vmem_unmap(iterator->mem, vaddr, KiB(4));     

            //Mark the end of the pipe as closed
            if(pipe_info->owner_process_id == core_descs->cur_task->pid)
                pipe_info->owner_process_id = 0;
            else if(pipe_info->user_process_id == core_descs->cur_task->pid)
                pipe_info->user_process_id = 0;

            //Remove and free the descriptor
            list_remove(&iterator->pipes, i);
            free(desc);

            //if the other end is also closed, delete the pipe
            if(pipe_info->owner_process_id == 0 && pipe_info->user_process_id == 0){
                delete_pipe(pipe_info, path, pipe_name);
            }

            local_spinlock_unlock(&process_lock);
            return CS_OK;
        }
    }

    local_spinlock_unlock(&process_lock);
    return CS_UNKN; //pipe does not exist
}
    
cs_error create_capability_syscall(const char *capability_name){
    local_spinlock_lock(&process_lock);
    process_desc_t *iterator = processes;
    do
        iterator = iterator->next;
    while(iterator->id != core_descs->cur_task->pid);

    //Check the list of capabilities of the current process to see if it already exists
    local_spinlock_lock(&iterator->lock);
    for(uint64_t i = 0; i < list_len(&iterator->owned_caps); i++){
        owned_cap_t *tmp = (owned_cap_t*)list_at(&iterator->owned_caps, i);
        if(strncmp(tmp->name, capability_name, TASK_NAME_LEN) == 0){
            local_spinlock_unlock(&iterator->lock);
            local_spinlock_unlock(&process_lock);
            return CS_UNKN; //Capability already exists
        }
    }
    local_spinlock_unlock(&iterator->lock);

    owned_cap_t *cap = malloc(sizeof(owned_cap_t));
    memset(cap, 0, sizeof(owned_cap_t));
    strncpy(cap->name, capability_name, TASK_NAME_LEN);
    if(list_init(&cap->shared_processes) != 0){
        free(cap);
        local_spinlock_unlock(&process_lock);
        return CS_OUTOFMEM;
    }

    local_spinlock_lock(&iterator->lock);
    list_append(&iterator->owned_caps, cap);
    local_spinlock_unlock(&iterator->lock);

    local_spinlock_unlock(&process_lock);
    return CS_OK;
}

cs_error share_capability_syscall(cs_id dst, const char *capability_name){
    local_spinlock_lock(&process_lock);
    process_desc_t *iterator = processes;
    do
        iterator = iterator->next;
    while(iterator->id != core_descs->cur_task->pid);

    process_desc_t *check_iter = processes;
    do
        check_iter = check_iter->next;
    while(check_iter->id != dst && check_iter->next != processes);
    if(check_iter->id != dst)
    {
        local_spinlock_unlock(&process_lock);
        return CS_UNKN; //Process id does not exist
    }

    //Check the list of capabilities of the current process to see if it already exists
    local_spinlock_lock(&iterator->lock);
    for(uint64_t i = 0; i < list_len(&iterator->owned_caps); i++){
        owned_cap_t *tmp = (owned_cap_t*)list_at(&iterator->owned_caps, i);
        if(strncmp(tmp->name, capability_name, TASK_NAME_LEN) == 0){
            list_append(&tmp->shared_processes, (void*)(uint64_t*)dst);

            local_spinlock_unlock(&iterator->lock);
            local_spinlock_unlock(&process_lock);
            return CS_OK; //Capability made available to process
        }
    }
    local_spinlock_unlock(&iterator->lock);
    local_spinlock_unlock(&process_lock);
    return CS_UNKN; //Capability does not exist
}

cs_error create_task_syscall(cs_task_type tasktype, char *name, cs_id *id) {
    return create_task_kernel(tasktype, name, task_permissions_none, id);
}
cs_error start_task_syscall(cs_id id, void(*handler)(void *arg)) {
    return start_task_kernel(id, handler);
}
cs_error exit_syscall() {
    //Release ownership of all pipes
    //Delete all capabilities
    return CS_OK;
}
cs_error kill_task_syscall() {
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
    cs_error ss_err = create_task_kernel(cs_task_type_process, "servicescript", task_permissions_kernel, &ss_id);
    if(ss_err != CS_OK)
        PANIC("SS_ERR0");
    ss_err = start_task_kernel(ss_id, servicescript_handler);
    if(ss_err != CS_OK)
        PANIC("SS_ERR1");

    if(timer_request(timer_features_periodic | timer_features_local, 50000, task_switch_handler))
        PANIC("Failed to allocate periodic timer!");

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
    syscall_sethandler(0, (void*)create_pipe_syscall);
    syscall_sethandler(1, (void*)open_pipe_syscall);
    syscall_sethandler(2, (void*)close_pipe_syscall);
    
    syscall_sethandler(10, (void*)create_capability_syscall);
    syscall_sethandler(11, (void*)share_capability_syscall);

    syscall_sethandler(20, (void*)create_task_syscall);
    syscall_sethandler(21, (void*)start_task_syscall);
    syscall_sethandler(22, (void*)exit_syscall);
    syscall_sethandler(23, (void*)kill_task_syscall);

    syscall_sethandler(30, (void*)nanosleep_syscall);

    //add map, unmap syscalls
    //syscall_sethandler(31, (void*)map_syscall);
    //syscall_sethandler(32, (void*)unmap_syscall);

    //TODO: consider adding code to SysDebug to allow it to provide support for user mode debuggers

    //Make sure that execution on the boot path doesn't continue past here.
    while(true)
        halt();

    return 0;
}