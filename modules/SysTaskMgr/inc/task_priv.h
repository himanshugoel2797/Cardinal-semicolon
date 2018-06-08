// Copyright (c) 2018 Himanshu Goel
// 
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINAL_SYSTASKMGR_PRIV_H
#define CARDINAL_SYSTASKMGR_PRIV_H

#include <stdint.h>

#define TASK_NAME_LEN 256

typedef enum {
    task_state_pending = 0,
    task_state_running,
    task_state_blocked,
    task_state_exiting,
    task_state_exited,
} task_state_t;

typedef enum {
    task_permissions_none = 0,
    task_permissions_memorymap = (1 << 0),
    task_permissions_io = (1 << 1),
    task_permissions_tasking = (1 << 2),
    task_permissions_interrupt = (1 << 3),
} task_permissions_t;

typedef struct task_desc {
    char name[TASK_NAME_LEN];
    task_state_t state;
    task_permissions_t permissions;
    uint64_t tid;
    uint64_t pid;
    uint64_t *stack;

    struct task_desc *next;
    struct task_desc *prev;
} task_desc_t;

typedef struct {
    uint64_t *kernel_stack;
    uint64_t *interrupt_stack;
    task_desc_t *cur_task;
} core_desc_t;

#endif