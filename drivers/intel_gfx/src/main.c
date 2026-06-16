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
#include "pci/pci_irq.h"

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
    int int_val = pci_setup_msi(device, interrupt_flags_none);

    if (int_val < 0)
        DEBUG_PRINT("NO MSI\r\n");

    interrupt_register_handler(int_val, intr_handler);

    //figure out which bar to use
    uint64_t bar = pci_first_mmio_bar(device);
    dev_state->bar_phys = (uintptr_t)bar;

    // Map enough of the register BAR (GTTMMADR) to reach the display registers.
    // The default 4 KiB only covers the very start; Cherrytrail's display block
    // begins at 0x180000 and Ironlake's PCH registers run past 0xC0000.
    size_t mmio_sz = KiB(4);

    if (dev_state->device->arch == IGFX_CHERRYTRAIL)
    {
        dev_state->display_mmio_base = IGFX_CHERRYTRAIL_DISP_BASE;
        dev_state->gtt_base = IGFX_CHERRYTRAIL_GTT_BASE;
        mmio_sz = MiB(2);
    }
    else if (dev_state->device->arch == IGFX_IRONLAKE)
    {
        dev_state->display_mmio_base = IGFX_IRONLAKE_DISP_BASE;
        mmio_sz = MiB(2);

        // BAR2 is the graphics memory aperture (GMADR); the firmware framebuffer
        // is visible through it. Capture its physical base for the hand-off path.
        uint64_t aperture = (device->bar[2] & 0xFFFFFFF0) + ((uint64_t)device->bar[3] << 32);
        dev_state->aperture_phys = (uintptr_t)aperture;
    }

    dev_state->bar = (uint8_t *)vmem_phystovirt((intptr_t)bar, mmio_sz, vmem_flags_rw | vmem_flags_uncached | vmem_flags_kernel);

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