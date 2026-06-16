/**
 * Copyright (c) 2018 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

#include "fpu.h"

void sysfp_register_tests(void);

int module_init() {
    fp_platform_init();
    sysfp_register_tests();
    return 0;
}

int fp_mp_init() {
    fp_platform_init();
    return 0;
}