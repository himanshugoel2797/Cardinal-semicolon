/**
 * Copyright (c) 2018 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

#include "fpu.h"

int module_init() {
    fp_platform_init();
    return 0;
}

int fp_mp_init() {
    fp_platform_init();
    return 0;
}