/**
 * Copyright (c) 2017 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */
#include "plat_defs.h"
#include "boot_information.h"

void sysuser_register_tests(void);

int module_init()
{
    //Setup syscall system
    syscall_plat_init();
    sysuser_register_tests();
    return 0;
}

int user_mp_init()
{
    syscall_plat_init();
    return 0;
}