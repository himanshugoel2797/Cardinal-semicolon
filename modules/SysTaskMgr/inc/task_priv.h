// Copyright (c) 2018 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINAL_SYSTASKMGR_PRIV_H
#define CARDINAL_SYSTASKMGR_PRIV_H

#include <stdint.h>
#include <stdlist.h>

#include "SysVirtualMemory/vmem.h"
#include "SysTaskMgr/task.h"
#include "thread.h"

#define TASK_NAME_LEN 256
#define KERNEL_STACK_LEN KiB(32)

typedef enum {
    task_state_uninitialized = 0,
    task_state_pending,
    task_state_running,
    task_state_suspended,
    task_state_blocked,
    task_state_exiting,
    task_state_exited,
} task_state_t;

typedef enum {
    pipe_flags_none = 0,
    pipe_flags_read = 1 << 1,
    pipe_flags_write = 1 << 2,
    pipe_flags_nocap = 1 << 3,
} pipe_flags_t;

typedef uint64_t pipe_t;

typedef struct {
    char name[TASK_NAME_LEN];
    list_t shared_processes;
} owned_cap_t;

typedef struct {
    pipe_t id;
    cs_id owner_process_id;
    cs_id user_process_id;
    pipe_flags_t flags;
    uint32_t sz;
    uintptr_t* pages;
    char name[TASK_NAME_LEN];
    char cap_name[TASK_NAME_LEN];
} pipe_info_t;

typedef struct {
    char proc_name[TASK_NAME_LEN];
    char pipe_name[TASK_NAME_LEN];
    uint64_t descriptor;
    intptr_t addr;
} pipe_descriptor_t;

typedef struct process_desc {
    char name[TASK_NAME_LEN];
    vmem_t *mem;
    cs_id id;
    int lock;
    int thd_cnt;

    //List of owned capabilities
    list_t owned_caps;

    //List of owned pipes
    list_t pipes;
    uint64_t desc_id;
    intptr_t pipe_base_vmem;

    struct process_desc *next;
    struct process_desc *prev;
} process_desc_t;

typedef struct task_desc {
    char name[TASK_NAME_LEN];
    task_state_t state;
    task_permissions_t permissions;
    int lock;

    cs_id pid;
    cs_id id;
    uint8_t *fpu_state;
    uint8_t *reg_state;
    uint8_t *kernel_stack;
    uint64_t sleep_starttime;

    struct task_desc *next;
    struct task_desc *prev;
} task_desc_t;

typedef struct {
    uint8_t *interrupt_stack;
    task_desc_t *cur_task;
} core_desc_t;

#endif