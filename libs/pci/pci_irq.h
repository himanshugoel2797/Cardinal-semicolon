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

// Like pci_setup_msi, but registers `handler` for the allocated vector BEFORE
// enabling the device's MSI/MSI-X capability. pci_setmsiinfo() turns the
// capability on, after which the device may immediately emit a message for an
// already-pending cause; if the handler isn't registered yet that first message
// is delivered to an unhandled vector and dropped, and an edge-triggered MSI(-X)
// then never re-fires -- wedging interrupts for the whole session. Registering
// first closes that race. The caller must still enable its device's *own*
// interrupt mask (e.g. the NIC IMR) only after this returns. Returns the
// allocated vector (>= 0), or -1 if the device has no MSI capability.
static inline int pci_setup_msi_handler(pci_config_t *device, interrupt_flags_t flags, InterruptHandler handler)
{
    int int_cnt = 0;
    int msi_val = pci_getmsiinfo(device, &int_cnt);
    if (msi_val < 0)
        return -1;

    int int_val = 0;
    interrupt_allocate(1, flags, &int_val);
    interrupt_register_handler(int_val, handler);   //register BEFORE enabling the cap

    uintptr_t msi_addr = (uintptr_t)interrupt_msi_register_addr(0);
    uint32_t msi_msg = interrupt_msi_register_data(int_val);
    pci_setmsiinfo(device, msi_val, &msi_addr, &msi_msg, 1);   //enables MSI(-X)

    return int_val;
}

#endif
