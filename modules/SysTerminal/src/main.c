/**
 * Copyright (c) 2018 Himanshu Goel
 * 
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */
#include "SysVirtualMemory/vmem.h"
#include "terminal_priv.h"

static terminaldef_t terminals;

int module_init(){

    terminals.uid = 0;
    terminals.tid = 0;
    if(vmem_create(&terminals.map) != 0)
        return -1;
    terminals.u_stack = NULL;
    terminals.k_stack = NULL;
    terminals.float_state = NULL;
    terminals.reg_state = NULL;
    terminals.children = NULL;

    return 0;
}