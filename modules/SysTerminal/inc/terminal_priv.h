// Copyright (c) 2018 Himanshu Goel
// 
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINAL_TERMINAL_PRIV_H
#define CARDINAL_TERMINAL_PRIV_H

#include <stdint.h>

#include "SysVirtualMemory/vmem.h"

typedef struct terminaldef {
    uint32_t uid;
    uint32_t tid;
    void *u_stack;
    void *k_stack;
    void *float_state;
    void *reg_state;
    vmem_t *map;
    struct terminaldef *children;
} terminaldef_t;

#endif