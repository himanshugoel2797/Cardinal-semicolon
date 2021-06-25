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

static void NORETURN kernel_entry_handler(void *handler, void *arg)
{
    ((void(*)(void*))handler)(arg);
    end_task_kernel(task_current());
    while(true)
        task_yield();
}

cs_error start_task_kernel(cs_id id, void *handler, void *arg)
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

                if (iter->permissions == task_permissions_none)
                {
                    iter->user_stack = (uint8_t *)0x100000000;
                    iter->user_stack += USER_STACK_LEN - sizeof(struct cardinal_program_setup_params);

                    uintptr_t pmem = pagealloc_alloc(0, 0, physmem_alloc_flags_data | physmem_alloc_flags_zero, USER_STACK_LEN);
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

cs_error end_task_kernel(cs_id id)
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
            DEBUG_PRINT("[SysTaskMgr] Process Exited: ");
            DEBUG_PRINT(iter->name);
            DEBUG_PRINT("\r\n");

            iter->state = task_state_exited; //Set task to exited

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

cs_error create_task_syscall(char *name, cs_id *id)
{
    return create_task_kernel(name, task_permissions_none, id);
}

cs_error start_task_syscall(cs_id id, void (*handler)(void *arg), void *arg)
{
    return start_task_kernel(id, handler, arg);
}

cs_error end_task_syscall()
{
    int cli_state = cli();
    cs_error retVal = end_task_kernel(task_current());
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
            //Restart iteration along sub table
            i = -1;
            d = d[i].desc_entry;
            id += i;
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
        if (core_descs->cur_task->syscall_data != NULL)
            syscall_getfullstate(core_descs->cur_task->syscall_data);

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
            }else if (ntask->state == task_state_suspended_monitor_mem_32){
                if(*ntask->monitor_tgt != ntask->monitor_value){
                    local_spinlock_unlock(&cur_ntask->lock);
                    break;
                }
            }else if(ntask->state == task_state_sleep) {
                if(timer_timestamp_ns() >= ntask->sleep_end){
                    local_spinlock_unlock(&cur_ntask->lock);
                    break;
                }
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
            }else if (ntask->state == task_state_suspended_monitor_mem_32){
                if(*ntask->monitor_tgt != ntask->monitor_value){
                    local_spinlock_unlock(&cur_ntask->lock);
                    break;
                }
            }else if(ntask->state == task_state_sleep) {
                if(timer_timestamp_ns() >= ntask->sleep_end){
                    local_spinlock_unlock(&cur_ntask->lock);
                    break;
                }
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
    ntask->state = task_state_running;
    vmem_setactive(ntask->mem);             //Set virtual memory
    fp_platform_setstate(ntask->fpu_state); //Set fpu state
    mp_platform_setstate(ntask->reg_state); //Set registers
    if (ntask->syscall_data != NULL)
        syscall_setfullstate(ntask->syscall_data);

    local_spinlock_unlock(&ntask->lock);

    local_spinlock_unlock(&process_lock);
    sti(cli_state);
}

static void task_yield_stage2(interrupt_register_state_t *mp_state){
    local_spinlock_lock(&process_lock);

    process_desc_t *ntask = NULL; //find the first pending task
    if (core_descs->cur_task != NULL)
    {
        local_spinlock_lock(&core_descs->cur_task->lock);

        if (core_descs->cur_task->state == task_state_running)
            core_descs->cur_task->state = task_state_pending;  //Set the cur_task to pending again
        fp_platform_getstate(core_descs->cur_task->fpu_state); //Save the current tasks's fpu state
        memcpy(core_descs->cur_task->reg_state, mp_state, sizeof(interrupt_register_state_t)); //Save the current task's register state
        if (core_descs->cur_task->syscall_data != NULL)
            syscall_getfullstate(core_descs->cur_task->syscall_data);

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
            }else if (ntask->state == task_state_suspended_monitor_mem_32){
                if(*ntask->monitor_tgt != ntask->monitor_value){
                    local_spinlock_unlock(&cur_ntask->lock);
                    break;
                }
            }else if(ntask->state == task_state_sleep) {
                if(timer_timestamp_ns() >= ntask->sleep_end){
                    local_spinlock_unlock(&cur_ntask->lock);
                    break;
                }
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
            }else if (ntask->state == task_state_suspended_monitor_mem_32){
                if(*ntask->monitor_tgt != ntask->monitor_value){
                    local_spinlock_unlock(&cur_ntask->lock);
                    break;
                }
            }else if(ntask->state == task_state_sleep) {
                if(timer_timestamp_ns() >= ntask->sleep_end){
                    local_spinlock_unlock(&cur_ntask->lock);
                    break;
                }
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
    ntask->state = task_state_running;
    vmem_setactive(ntask->mem);             //Set virtual memory
    fp_platform_setstate(ntask->fpu_state); //Set fpu state
    //mp_platform_setstate(ntask->reg_state); //Set registers
    memcpy(mp_state, ntask->reg_state, sizeof(interrupt_register_state_t));
    if (ntask->syscall_data != NULL)
        syscall_setfullstate(ntask->syscall_data);

    local_spinlock_unlock(&ntask->lock);

    local_spinlock_unlock(&process_lock);
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
    local_spinlock_lock(&process_lock);
    if (processes != NULL)
    {
        cs_error res_cs = CS_UNKN;
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
            int res = vmem_virttophys(iter->mem, vaddr, phys);
            if (res == 0)
                res_cs = CS_OK;

            //Lock is already held from the break in the previous loop
            local_spinlock_unlock(&iter->lock);
        }
        local_spinlock_unlock(&process_lock);
        sti(cli_state);
        return res_cs;
    }
    local_spinlock_unlock(&process_lock);
    sti(cli_state);
    return CS_UNKN;
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
    local_spinlock_lock(&process_lock);
    if (processes != NULL)
    {
        cs_error res_cs = CS_UNKN;
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
            intptr_t phys_ptr = 0;
            vmem_virttophys(iter->mem, (intptr_t)tgt, &phys_ptr);
            iter->monitor_tgt = (uint32_t*)vmem_phystovirt(phys_ptr, 4, vmem_flags_uncached | vmem_flags_kernel);
            iter->monitor_value = cur_val;
            iter->state = task_state_suspended_monitor_mem_32;

            //Lock is already held from the break in the previous loop
            local_spinlock_unlock(&iter->lock);
        }
        local_spinlock_unlock(&process_lock);
        sti(cli_state);
        return res_cs;
    }
    local_spinlock_unlock(&process_lock);
    sti(cli_state);
    return CS_UNKN;
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
                uintptr_t pmem = pagealloc_alloc(0, 0, physmem_alloc_flags_data | physmem_alloc_flags_instr | physmem_alloc_flags_zero, sz);
                d->map_entry->paddr = pmem;
                d->map_entry->is_owner = true;
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

cs_error task_allocdescriptor(cs_id id, DescriptorResourceFreeAction action, void *state NULLABLE, cs_id *descriptor NULLABLE)
{
    if (action == NULL)
        return CS_UNKN;

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
        local_spinlock_unlock(&process_lock);
        sti(cli_state);
        return CS_OK;
    }
    local_spinlock_unlock(&process_lock);
    sti(cli_state);
    return CS_UNKN;
}

cs_error task_freedescriptor(cs_id id, cs_id descriptor)
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
        local_spinlock_unlock(&process_lock);
        sti(cli_state);
        return CS_OK;
    }
    local_spinlock_unlock(&process_lock);
    sti(cli_state);
    return CS_UNKN;
}

cs_error task_sleep(cs_id id, uint64_t ns)
{
    int cli_state = cli();
    local_spinlock_lock(&process_lock);
    if (processes != NULL)
    {
        cs_error res_cs = CS_UNKN;
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
            iter->sleep_end = timer_timestamp_ns() + ns;
            iter->state = task_state_sleep;

            //Lock is already held from the break in the previous loop
            local_spinlock_unlock(&iter->lock);
        }
        local_spinlock_unlock(&process_lock);
        sti(cli_state);
        return res_cs;
    }
    local_spinlock_unlock(&process_lock);
    task_yield();
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
                    if (iter->syscall_data != NULL)
                        free(iter->syscall_data);
                    if (iter->user_stack != NULL)
                    {
                        vmem_unmap(iter->mem, 0x100000000, USER_STACK_LEN);
                        pagealloc_free(iter->user_stack_phys, USER_STACK_LEN);
                    }
                    vmem_destroy(iter->mem);
                    free(iter->fpu_state_unaligned);
                    free(iter->reg_state);
                    free(iter->kernel_stack - KERNEL_STACK_LEN);

                    iter->mem = NULL;
                }

                if (prev_iter == NULL)
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

void semaphore_init(semaphore_t *sema)
{
    sema->count = 0;
    sema->spinlock = 0;
}

int semaphore_signal(semaphore_t *sema)
{
    int rVal = 0;
    local_spinlock_lock(&sema->spinlock);
    rVal = sema->count++;
    local_spinlock_unlock(&sema->spinlock);
    return rVal;
}

int semaphore_wait(semaphore_t *sema)
{
    int rVal = 0;
    local_spinlock_lock(&sema->spinlock);
    while (sema->count == 0){
        int state = cli();
        task_monitor_noyield(task_current(), (uint32_t*)&sema->count, 0);
        local_spinlock_unlock(&sema->spinlock);
        task_yield();
        sti(state);
        local_spinlock_lock(&sema->spinlock);
    }
    rVal = --sema->count;
    local_spinlock_unlock(&sema->spinlock);
    return rVal;
}

int servicescript_execute();
void servicescript_handler(void *arg)
{
    arg = NULL;
    servicescript_execute();
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

void task_startnew_user(void *elf, size_t elf_len)
{
    cs_id elf_id = 0;
    cs_error elf_err = create_task_kernel("elf_test", task_permissions_none, &elf_id);
    if (elf_err != CS_OK)
        PANIC("[SysTaskMgr] Failed to create elf_test task.");

    void (*entry_pt)(void *) = NULL;
    user_elf_load(elf_id, elf, elf_len, &entry_pt);
    elf_err = start_task_kernel(elf_id, entry_pt, NULL);
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
    syscall_sethandler(7, (void *)end_task_syscall);

    syscall_sethandler(8, (void *)openspecialset_syscall); //Request a special set of syscalls to be enabled for this process

    //TODO: consider adding code to SysDebug to allow it to provide support for user mode debuggers

    //Make sure that execution on the boot path doesn't continue past here.
    while (true)
        halt();

    task_cleanup(NULL);

    return 0;
}