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
#include "task_priv.h"

static task_desc_t *tasks = NULL;
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

    //Install ipc, get_perms, release_perms syscalls

    return 0;
}