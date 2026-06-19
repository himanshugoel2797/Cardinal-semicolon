/**
 * Copyright (c) 2017 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

#include "SysMemory/memory.h"

void *stack_alloc(size_t sz, bool kernel) {
    sz = 0;
    kernel = 0;
    return NULL;
}


int kernel_updatememhandlers();

int module_init() {
    return 0;
}

int mem_init() {
    kernel_updatememhandlers();
    return 0;
}

int mem_mp_init() {
    return 0;
}