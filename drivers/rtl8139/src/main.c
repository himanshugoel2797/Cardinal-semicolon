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

#include "state.h"

int module_init(void *ecam_addr) {

    pci_config_t *device = (pci_config_t*)vmem_phystovirt((intptr_t)ecam_addr, KiB(4), vmem_flags_uncached | vmem_flags_kernel | vmem_flags_rw);

    //enable pci bus master
    device->command.busmaster = 1;

    //interrupt setup
    int int_cnt = 0;
    int msi_val = pci_getmsiinfo(device, &int_cnt);

    int int_val = 0;
    interrupt_allocate(1, interrupt_flags_none, &int_val);
    interrupt_registerhandler(int_val, rtl8139_intr_handler);

    uintptr_t msi_addr = (uintptr_t)msi_register_addr(0);
    uint32_t msi_msg = msi_register_data(int_val);
    pci_setmsiinfo(device, msi_val, &msi_addr, &msi_msg, 1);

    //The memory space bar for the registers
    uint64_t bar = (device->bar[1] & 0xFFFFFFF0);

    rtl8139_state_t *n_state = malloc(sizeof(rtl8139_state_t));
    memset(n_state, 0, sizeof(rtl8139_state_t));

    n_state->intrpt_num = int_val;
    n_state->memar = (uint8_t*)vmem_phystovirt((intptr_t)bar, KiB(4), vmem_flags_uncached | vmem_flags_kernel | vmem_flags_rw);

    rtl8139_init(n_state);

    return 0;
}