/**
 * Copyright (c) 2018 hgoel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */
#include <stdlib.h>
#include <string.h>
#include <types.h>

#include "ps2.h"

int module_init(void) {
    PS2_Initialize();
    return 0;
}