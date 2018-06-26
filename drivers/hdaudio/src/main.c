/**
 * Copyright (c) 2018 Himanshu Goel
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

static int device_init_lock = 0;

//TODO: design this driver such that it can handle multiple devices
int module_init(void *ecam_addr){

    local_spinlock_lock(&device_init_lock);
    pci_config_t *device = (pci_config_t*)vmem_phystovirt((intptr_t)ecam_addr, KiB(4), vmem_flags_uncached | vmem_flags_kernel | vmem_flags_rw);

    //enable pci bus master
    device->command.busmaster = 1;

    uint64_t bar = 0;

    //figure out which bar to use
    for(int i = 0; i < 6; i++) {
        if((device->bar[i] & 0x6) == 0x4) {
            //Is 64-bit
            bar = (device->bar[i] & 0xFFFFFFF0) + ((uint64_t)device->bar[i + 1] << 32);
        }else if((device->bar[i] & 0x6) == 0x0){
            //Is 32-bit
            bar = (device->bar[i] & 0xFFFFFFF0);
        }

        if(bar != 0)
            break;
    }

    DEBUG_PRINT("HD Audio Initialized\r\n");
    local_spinlock_unlock(&device_init_lock);

    return 0;
}