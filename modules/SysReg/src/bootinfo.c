/**
 * Copyright (c) 2017 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */
#include "boot_information.h"
#include "registry.h"
#include <stdlib.h>
#include <string.h>
#include <types.h>

PRIVATE int add_bootinfo() {
    CardinalBootInfo *bInfo = GetBootInfo();

    if (registry_addkey_uint("HW/BOOTINFO", "MEMSIZE", bInfo->MemorySize) !=
            CS_OK)
        return -1;

    if (registry_addkey_uint("HW/BOOTINFO", "RSDPADDR", bInfo->RSDPAddress) !=
            CS_OK)
        return -2;

    {
        if (registry_createdirectory("HW/BOOTINFO", "INITRD") != CS_OK)
            return -3;

        if (registry_addkey_uint("HW/BOOTINFO/INITRD", "VIRT_ADDR",
                                 bInfo->InitrdStartAddress) != CS_OK)
            return -4;

        if (registry_addkey_uint("HW/BOOTINFO/INITRD", "PHYS_ADDR",
                                 bInfo->InitrdPhysStartAddress) != CS_OK)
            return -4;

        if (registry_addkey_uint("HW/BOOTINFO/INITRD", "LEN",
                                 bInfo->InitrdLength) != CS_OK)
            return -5;
    }

    // Physical memory info
    {

        uint32_t j = 0;
        for (uint32_t i = 0; i < bInfo->CardinalMemoryMapCount; i++) {

            if (bInfo->CardinalMemoryMap[i].type != CardinalMemoryRegionType_Free)
                continue;

            char idx_str[10];
            char key_str[256] = "HW/PHYS_MEM/";
            char *key_idx = strncat(key_str, itoa(j, idx_str, 16), 255);

            if (registry_createdirectory("HW/PHYS_MEM", idx_str) != CS_OK)
                return -6;

            if (registry_addkey_uint(key_idx, "ADDR",
                                     bInfo->CardinalMemoryMap[i].addr) !=
                    CS_OK)
                return -7;

            if (registry_addkey_uint(key_idx, "LEN",
                                     bInfo->CardinalMemoryMap[i].len) !=
                    CS_OK)
                return -8;

            j++;
        }

        if (registry_addkey_uint("HW/PHYS_MEM", "ENTRY_COUNT", j) !=
                CS_OK)
            return -9;
    }

    // Boot time framebuffer
    {
        if (registry_createdirectory("HW/BOOTINFO", "FRAMEBUFFER") !=
                CS_OK)
            return -10;

        if (registry_addkey_uint("HW/BOOTINFO/FRAMEBUFFER", "PHYS_ADDR",
                                 bInfo->FramebufferAddress) != CS_OK)
            return -11;

        if (registry_addkey_uint("HW/BOOTINFO/FRAMEBUFFER", "PITCH",
                                 bInfo->FramebufferPitch) != CS_OK)
            return -12;

        if (registry_addkey_uint("HW/BOOTINFO/FRAMEBUFFER", "WIDTH",
                                 bInfo->FramebufferWidth) != CS_OK)
            return -13;

        if (registry_addkey_uint("HW/BOOTINFO/FRAMEBUFFER", "HEIGHT",
                                 bInfo->FramebufferHeight) != CS_OK)
            return -14;

        if (registry_addkey_uint("HW/BOOTINFO/FRAMEBUFFER", "RED_MASK",
                                 ((1 << bInfo->FramebufferRedMaskSize) - 1)))
            return -15;

        if (registry_addkey_uint("HW/BOOTINFO/FRAMEBUFFER", "RED_OFFSET",
                                 bInfo->FramebufferRedFieldPosition))
            return -16;

        if (registry_addkey_uint("HW/BOOTINFO/FRAMEBUFFER", "GREEN_MASK",
                                 ((1 << bInfo->FramebufferGreenMaskSize) - 1)))
            return -17;

        if (registry_addkey_uint("HW/BOOTINFO/FRAMEBUFFER", "GREEN_OFFSET",
                                 bInfo->FramebufferGreenFieldPosition))
            return -18;

        if (registry_addkey_uint("HW/BOOTINFO/FRAMEBUFFER", "BLUE_MASK",
                                 ((1 << bInfo->FramebufferBlueMaskSize) - 1)))
            return -19;

        if (registry_addkey_uint("HW/BOOTINFO/FRAMEBUFFER", "BLUE_OFFSET",
                                 bInfo->FramebufferBlueFieldPosition))
            return -20;
    }
    return 0;
}