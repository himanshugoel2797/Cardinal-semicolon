/**
 * Copyright (c) 2017 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

#include "page_allocator.h"
#include "SysPhysicalMemory/phys_mem.h"
#include <stdlib.h>

int module_init() {
    pagealloc_init();
    return 0;
}

int pmem_initial_test() {
    DEBUG_ECHO("Starting SysPhysicalMemory test set 1...\r\n");

    for(int i = 0; i < 32; i++) {
        char tmp_buf[10];
        DEBUG_PRINT(itoa((int)pagealloc_alloc(0, 0, 0, KiB(4)), tmp_buf, 16));
        DEBUG_PRINT("\r\n");
    }

    DEBUG_ECHO("Test passed!\r\n");

    return 0;
}