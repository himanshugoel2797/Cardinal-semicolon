// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINAL_PCI_DEBUG_H
#define CARDINAL_PCI_DEBUG_H

// Reusable PCI debug helpers (DEBUG_PRINT-based). Kept separate from pci.h so
// normal users don't pull in the debug output dependency.

#include "pci.h"
#include <stdlib.h>

// Dump a device's MSI-X capability + table entry 0 over DEBUG_PRINT: the
// enable / function-mask bits, the table BIR/address, and the programmed message
// address/data + per-vector control read back from the table BAR. The fast way
// to see why MSI-X isn't delivering on a given device:
//   addrLo=0x0          -> the table write never reached the BAR (wrong BIR, or
//                          an unreachable/closed bridge window over the BAR)
//   vctl bit0 == 1      -> the vector is masked
//   addrLo=0xFEExxxxx, en=1, vctl bit0==0 -> table is correct; the fault is
//                          upstream message routing (bridge bus-master / LAPIC)
// Prints nothing if the device has no MSI-X capability.
static inline void pci_msix_debug_dump(pci_config_t *device)
{
    if (device->capabilitiesPtr == 0)
        return;

    uint8_t ptr = device->capabilitiesPtr;
    uint8_t *base = (uint8_t *)device;
    while (ptr != 0)
    {
        pci_cap_header_t *cap = (pci_cap_header_t *)(base + ptr);
        if (cap->capID == pci_cap_msix)
        {
            pci_msix_t *mx = (pci_msix_t *)cap;
            char t[20];
            int bir = mx->table_off.bir;            //3-bit field; only 0..5 are valid BARs
            if (bir >= 6)
                return;
            uint64_t bar = device->bar[bir] & 0xFFFFFFF0;
            if ((device->bar[bir] & 0x6) == 0x4)
                bar |= ((uint64_t)device->bar[bir + 1] << 32);
            uint64_t tphys = bar + ((uint64_t)mx->table_off.offset << 3);
            volatile uint32_t *tbl = (volatile uint32_t *)vmem_phystovirt(
                (intptr_t)tphys, KiB(4),
                vmem_flags_uncached | vmem_flags_kernel | vmem_flags_rw);

            DEBUG_PRINT("[pci] MSI-X en=");
            DEBUG_PRINT(itoa(mx->ctrl.enable, t, 10));
            DEBUG_PRINT(" fmask=");
            DEBUG_PRINT(itoa(mx->ctrl.func_mask, t, 10));
            DEBUG_PRINT(" bir=");
            DEBUG_PRINT(itoa(bir, t, 10));
            DEBUG_PRINT(" tbl@0x");
            DEBUG_PRINT(ltoa((long long)tphys, t, 16));
            DEBUG_PRINT(" [addrLo=0x");
            DEBUG_PRINT(itoa(tbl[0], t, 16));
            DEBUG_PRINT(" addrHi=0x");
            DEBUG_PRINT(itoa(tbl[1], t, 16));
            DEBUG_PRINT(" data=0x");
            DEBUG_PRINT(itoa(tbl[2], t, 16));
            DEBUG_PRINT(" vctl=0x");
            DEBUG_PRINT(itoa(tbl[3], t, 16));
            DEBUG_PRINT("]\r\n");
        }
        ptr = cap->nextPtr;
    }
}

#endif
