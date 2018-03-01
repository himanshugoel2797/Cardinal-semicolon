// Copyright (c) 2018 Himanshu Goel
// 
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINAL_TERMINAL_PRIV_H
#define CARDINAL_TERMINAL_PRIV_H

#include <stdint.h>

#include "SysVirtualMemory/vmem.h"

typedef enum {
    terminalflag_kernel = (1ull << 0),
} terminalflags_t;

typedef enum {
    terminalstate_pending = 0,
    terminalstate_running = 1,
    terminalstate_blocked = 2,
    terminalstate_exiting = 3,
} terminalstate_t;

typedef struct terminaldef {
    uint32_t uid;
    uint32_t tid;
    terminalflags_t flags;
    terminalstate_t state;
    int lock;
    void *u_stack;
    void *k_stack;
    void *float_state;
    void *reg_state;
    vmem_t *map;

    //This pointer has less strict requirements,
    //it can be touched as long as the queue is locked
    struct terminaldef *next;
} terminaldef_t;

#endif