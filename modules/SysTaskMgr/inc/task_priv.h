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
#include "ipc.h"

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
    obj_entrytype_capability,
    obj_entrytype_indirect,
} obj_entrytype_t;

typedef struct {
    int func_cnt;
    cs_id proc_id;
    cs_func_t funcs[0];
} obj_desc_t;

typedef struct process_desc {
    char name[TASK_NAME_LEN];
    vmem_t *mem;
    cs_id id;
    int lock;

    task_state_t state;
    task_permissions_t permissions;

    uint8_t *fpu_state;
    uint8_t *reg_state;
    uint8_t *kernel_stack;

    struct process_desc *next;
    struct process_desc *prev;
} process_desc_t;

typedef struct {
    uint8_t *interrupt_stack;
    process_desc_t *cur_task;
} core_desc_t;

#endif