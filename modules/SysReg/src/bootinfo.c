/**
 * Copyright (c) 2017 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */
#include "boot_information.h"
#include "registry.h"
#include <types.h>

PRIVATE int add_bootinfo() {
  CardinalBootInfo *bInfo = GetBootInfo();

  if (registry_addkey_uint("HW/BOOTINFO", "MEMSIZE", bInfo->MemorySize) !=
      registry_err_ok)
    return -1;

  if (registry_addkey_uint("HW/BOOTINFO", "RSDPADDR", bInfo->RSDPAddress) !=
      registry_err_ok)
    return -1;

  {
    if (registry_createdirectory("HW/BOOTINFO", "INITRD") != registry_err_ok)
      return -1;

    if (registry_addkey_uint("HW/BOOTINFO/INITRD", "PHYS_ADDR",
                             bInfo->InitrdStartAddress) != registry_err_ok)
      return -1;

    if (registry_addkey_uint("HW/BOOTINFO/INITRD", "LEN",
                             bInfo->InitrdLength) != registry_err_ok)
      return -1;
  }

  // Physical memory info
  {}

  // Boot time framebuffer
  {
    if (registry_createdirectory("HW/BOOTINFO", "FRAMEBUFFER") !=
        registry_err_ok)
      return -1;

    if (registry_addkey_uint("HW/BOOTINFO/FRAMEBUFFER", "PHYS_ADDR",
                             bInfo->FramebufferAddress) != registry_err_ok)
      return -1;

    if (registry_addkey_uint("HW/BOOTINFO/FRAMEBUFFER", "PITCH",
                             bInfo->FramebufferPitch) != registry_err_ok)
      return -1;

    if (registry_addkey_uint("HW/BOOTINFO/FRAMEBUFFER", "WIDTH",
                             bInfo->FramebufferWidth) != registry_err_ok)
      return -1;

    if (registry_addkey_uint("HW/BOOTINFO/FRAMEBUFFER", "HEIGHT",
                             bInfo->FramebufferHeight) != registry_err_ok)
      return -1;

    if (registry_addkey_uint("HW/BOOTINFO/FRAMEBUFFER", "RED_MASK",
                             ((1 << bInfo->FramebufferRedMaskSize) - 1)))
      return -1;

    if (registry_addkey_uint("HW/BOOTINFO/FRAMEBUFFER", "RED_OFFSET",
                             bInfo->FramebufferRedFieldPosition))
      return -1;

    if (registry_addkey_uint("HW/BOOTINFO/FRAMEBUFFER", "GREEN_MASK",
                             ((1 << bInfo->FramebufferGreenMaskSize) - 1)))
      return -1;

    if (registry_addkey_uint("HW/BOOTINFO/FRAMEBUFFER", "GREEN_OFFSET",
                             bInfo->FramebufferGreenFieldPosition))
      return -1;

    if (registry_addkey_uint("HW/BOOTINFO/FRAMEBUFFER", "BLUE_MASK",
                             ((1 << bInfo->FramebufferBlueMaskSize) - 1)))
      return -1;

    if (registry_addkey_uint("HW/BOOTINFO/FRAMEBUFFER", "BLUE_OFFSET",
                             bInfo->FramebufferBlueFieldPosition))
      return -1;
  }
  return 0;
}