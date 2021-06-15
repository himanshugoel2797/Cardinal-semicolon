/**
 * Copyright (c) 2018 hgoel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */
#include <stdlib.h>
#include <string.h>
#include <types.h>

#include "SysReg/registry.h"

#include "ps2.h"

int module_init(void)
{
    bool ps2_pres = false;
    registry_readkey_bool("HW/FADT", "8042", &ps2_pres);
    if (ps2_pres)
        PS2_Initialize();
    else
        DEBUG_PRINT("[PS/2] Controller Missing, exiting.\r\n");
    return 0;
}