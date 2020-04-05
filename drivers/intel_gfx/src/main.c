/**
 * Copyright (c) 2018 hgoel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */
#include <types.h>
#include <stdlib.h>
#include <string.h>

#include <cardinal/local_spinlock.h>

#include "SysVirtualMemory/vmem.h"
#include "SysPhysicalMemory/phys_mem.h"
#include "SysInterrupts/interrupts.h"
#include "pci/pci.h"

#include "devices.h"
#include "gmbus.h"
#include "display.h"
#include "gtt.h"

static void intr_handler(int intr_num)
{
    intr_num = 0;
}

int module_init(void *ecam_addr)
{

    pci_config_t *device = (pci_config_t *)vmem_phystovirt((intptr_t)ecam_addr, KiB(4), vmem_flags_uncached | vmem_flags_kernel | vmem_flags_rw);

    //enable pci bus master
    device->command.busmaster = 1;

    //identify the device
    igfx_dev_state_t *dev_state = malloc(sizeof(igfx_dev_state_t));
    if (igfx_getdevice(device->deviceID, &dev_state->device) != 0)
    {
        DEBUG_PRINT("[intel_gfx] Driver loaded for unsupported device!");
        return 0;
    }

    //interrupt setup
    int int_cnt = 0;
    int msi_val = pci_getmsiinfo(device, &int_cnt);

    if (msi_val < 0)
        DEBUG_PRINT("NO MSI\r\n");

    int int_val = 0;
    interrupt_allocate(1, interrupt_flags_none, &int_val);
    interrupt_registerhandler(int_val, intr_handler);

    uintptr_t msi_addr = (uintptr_t)msi_register_addr(0);
    uint32_t msi_msg = msi_register_data(int_val);
    pci_setmsiinfo(device, msi_val, &msi_addr, &msi_msg, 1);

    //figure out which bar to use
    uint64_t bar = 0;
    for (int i = 0; i < 6; i++)
    {
        if ((device->bar[i] & 0x6) == 0x4) //Is 64-bit
            bar = (device->bar[i] & 0xFFFFFFF0) + ((uint64_t)device->bar[i + 1] << 32);
        else if ((device->bar[i] & 0x6) == 0x0) //Is 32-bit
            bar = (device->bar[i] & 0xFFFFFFF0);
        if (bar)
            break;
    }
    dev_state->bar_phys = (uintptr_t)bar;
    dev_state->bar = (uint8_t *)vmem_phystovirt((intptr_t)bar, KiB(4), vmem_flags_rw | vmem_flags_uncached | vmem_flags_kernel);

    if (dev_state->device->arch == IGFX_CHERRYTRAIL)
    {
        dev_state->display_mmio_base = IGFX_CHERRYTRAIL_DISP_BASE;
        dev_state->gtt_base = IGFX_CHERRYTRAIL_GTT_BASE;
    }

    igfx_gmbus_init(dev_state);
    igfx_display_init(dev_state);

    //Start by enumerating all ports

    //Register ports to the driver

    //Disable all ports
    //Disable all planes
    //Disable display pipe
    //Disable DPLL
    //Disable VGA emulation

    //Enable DPLL, wait for stabalization

    //Initialize displays on all connected ports
    //Read EDIDs via GMBUS
    //Remove any display options that the gpu can't support
    //Choose the highest supported resolution
    //Callibrate it and bring the backlight pwm online
    //If an audio device is associated, pass along the info to CoreAudio

    //Configure display planes

    //Register the displays to CoreDisplay and start working on understanding the 3d documentation

    DEBUG_PRINT("igfx initialized!\r\n");

    return 0;
}