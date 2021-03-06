// Copyright (c) 2018 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINAL_SYSTASKMGR_PRIV_H
#define CARDINAL_SYSTASKMGR_PRIV_H

#include <stdint.h>
#include <stdlist.h>

#include "SysUser/syscall.h"
#include "SysObj/obj.h"
#include "SysVirtualMemory/vmem.h"
#include "SysTaskMgr/task.h"
#include "cs_syscall.h"

#define TASK_NAME_LEN 256
#define MAX_DESCRIPTOR_COUNT 256
#define KERNEL_STACK_LEN KiB(32)
#define USER_STACK_LEN KiB(32)

typedef enum
{
    task_state_uninitialized = 0,
    task_state_pending,
    task_state_running,
    task_state_suspended,
    task_state_suspended_monitor_mem_32,
    task_state_sleep,
    task_state_blocked,
    task_state_exited,
} task_state_t;

typedef enum
{
    descriptor_type_unused_entry = 0,
    descriptor_type_map_entry = 1,
    descriptor_type_descriptor_entry = 2, //Recursive descriptor set
    descriptor_type_resource_entry = 3,  //Describes a generic resource that may need to be freed on exit
} descriptor_type_t;

typedef struct map_entry
{
    intptr_t vaddr;
    uintptr_t paddr;
    size_t sz;
    task_map_perms_t owner_perms;
    task_map_perms_t child_perms;
    task_map_flags_t flags;
    bool is_owner;
    int child_count;
} map_entry_t;

typedef struct resource_entry{
    void *state;
    DescriptorResourceFreeAction action;
} resource_entry_t;

typedef struct descriptor_entry
{
    union {
        map_entry_t *map_entry;
        resource_entry_t *resource_entry;
        struct descriptor_entry *desc_entry;
    };
    descriptor_type_t type;
} descriptor_entry_t;

struct cardinal_program_setup_params
{
    uint16_t ver;
    uint16_t page_size;
    uint32_t argc;
    uint64_t pid;
    uint64_t rng_seed;
    uintptr_t entry_point;
    char **envp;
    char **argv;
};

typedef struct process_desc
{
    char name[TASK_NAME_LEN];
    vmem_t *mem;
    cs_id id;
    int lock;

    task_state_t state;
    task_permissions_t permissions;
    
    union{
        uint32_t monitor_value;
    };
    union{
        volatile uint32_t *monitor_tgt;
        uint64_t sleep_end;
    };

    descriptor_entry_t descriptors[MAX_DESCRIPTOR_COUNT];

    uint8_t *fpu_state;
    uint8_t *fpu_state_unaligned;
    uint8_t *reg_state;
    uint8_t *kernel_stack;
    uint8_t *user_stack;
    intptr_t user_stack_phys;
    uint8_t *syscall_data;

    struct cardinal_program_setup_params *usersetup_params;

    struct process_desc *next;
} process_desc_t;

typedef struct
{
    uint8_t *interrupt_stack;
    process_desc_t *cur_task;
} core_desc_t;

#endif