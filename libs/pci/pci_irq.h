// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINAL_PCI_IRQ_H
#define CARDINAL_PCI_IRQ_H

// MSI wiring helper: bridges the PCI config-space MSI capability (pci.h) and the
// interrupt allocator (SysInterrupts). Kept out of pci.h so plain config-space
// users don't pull in a dependency on the interrupt module.

#include "pci.h"
#include <SysInterrupts/interrupts.h>

// Allocate one interrupt vector and program the device's MSI capability to
// deliver to it on CPU 0. Returns the allocated vector (>= 0), or -1 if the
// device has no MSI capability.
static inline int pci_setup_msi(pci_config_t *device, interrupt_flags_t flags)
{
    int int_cnt = 0;
    int msi_val = pci_getmsiinfo(device, &int_cnt);
    if (msi_val < 0)
        return -1;

    int int_val = 0;
    interrupt_allocate(1, flags, &int_val);

    uintptr_t msi_addr = (uintptr_t)interrupt_msi_register_addr(0);
    uint32_t msi_msg = interrupt_msi_register_data(int_val);
    pci_setmsiinfo(device, msi_val, &msi_addr, &msi_msg, 1);

    return int_val;
}

#endif
