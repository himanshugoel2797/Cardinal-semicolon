/**
 * Copyright (c) 2017 Himanshu Goel
 * 
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */
#include "plat_defs.h"

int module_init(){
    //Setup syscall system
    syscall_plat_init();
    return 0;
}

int user_mp_init() {
    syscall_plat_init();
    return 0;
}