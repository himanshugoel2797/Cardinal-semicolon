/**
 * Copyright (c) 2018 hgoel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */
#include <types.h>

#include "SysReg/registry.h"

#include "ps2.h"

int module_init(void)
{
    // The FADT "IA-PC boot architecture" 8042 flag is the canonical presence hint,
    // but several boards (notably QEMU's q35) leave it clear even though an i8042
    // is present. PS2_Initialize self-tests the controller (expects 0x55) and bails
    // cleanly on truly-absent hardware -- reads from an unpopulated i8042 return
    // 0xFF -- so attempt init regardless and just log what the FADT claimed.
    bool ps2_pres = false;
    registry_readkey_bool("HW/FADT", "8042", &ps2_pres);
    if (!ps2_pres)
        DEBUG_PRINT("[PS/2] FADT does not advertise an 8042; probing anyway.\r\n");
    if (PS2_Initialize() != 0)
        DEBUG_PRINT("[PS/2] Controller self-test failed; no PS/2 input.\r\n");
    return 0;
}