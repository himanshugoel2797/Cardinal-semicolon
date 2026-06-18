// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINAL_PCI_ALLOC_H
#define CARDINAL_PCI_ALLOC_H

// General PCI MMIO-BAR + bridge-window allocation.
//
// Most devices come up with firmware-assigned BARs and reachable bridges, so the
// usual path is pci_first_mmio_bar() (pci.h). But a device that firmware never
// used at boot (e.g. an onboard NIC behind a PCIe root port) can be left with
// *unassigned* BARs AND a *closed* bridge memory window -- nothing reaches it.
// pci_assign_bars() handles that case: pick a free MMIO region above the existing
// assignments, program the device's BARs into it, and open the forwarding window
// on every bridge between the device and the root bus.
//
// Header-only inline, matching pci.h / pci_irq.h. Requires SysReg (to enumerate
// PCI devices) and SysVirtualMemory (pulled in via pci.h).

#include "pci.h"
#include <string.h>
#include <stdlib.h>
#include <SysReg/registry.h>

// PCI-to-PCI bridge memory base/limit registers are 1 MiB granular.
#define PCI_BRIDGE_GRAN 0x100000ULL

// Map a device's ECAM config space for read/write (uncached, kernel, rw).
static inline volatile void *pci_map_ecam(uint64_t ecam)
{
    return (volatile void *)vmem_phystovirt((intptr_t)ecam, KiB(4),
            vmem_flags_uncached | vmem_flags_kernel | vmem_flags_rw);
}

// Size BAR slot `i` in place: write all-ones, read back the decode mask, restore.
// 64-bit aware (sizes both halves of a 64-bit BAR). The caller must have the
// device's memory decode disabled. Returns the size in bytes, or 0 if the slot
// is unimplemented / is an I/O BAR.
static inline uint64_t pci_bar_getsize(volatile uint32_t *bars, int i)
{
    if (bars[i] & 0x1)
        return 0;                                   //I/O BAR
    bool b64 = ((bars[i] & 0x6) == 0x4);

    uint32_t o0 = bars[i];
    bars[i] = 0xFFFFFFFFu;
    uint64_t mask = (uint64_t)(bars[i] & 0xFFFFFFF0u);
    bars[i] = o0;
    if ((uint32_t)mask == 0)
        return 0;                                   //unimplemented

    if (b64 && i + 1 < 6)
    {
        uint32_t o1 = bars[i + 1];
        bars[i + 1] = 0xFFFFFFFFu;
        mask |= ((uint64_t)bars[i + 1] << 32);
        bars[i + 1] = o1;
    }
    else
    {
        mask |= 0xFFFFFFFF00000000ULL;              //32-bit BAR: upper bits implied set
    }
    return ~mask + 1;
}

// Highest end address (base+size) of any *assigned* memory BAR below `ceiling`,
// found by scanning every enumerated PCI device and sizing its BARs. Runs during
// driver load (before APs/poll tasks exist), so briefly toggling a neighbour's
// memory-decode to size its BARs is safe. Used to place a new BAR just above the
// firmware-assigned region (and thus inside the host-bridge aperture).
static inline uint64_t pci_mmio_used_top(uint64_t ceiling)
{
    uint64_t count = 0, top = 0;
    if (registry_readkey_uint("HW/PCI", "COUNT", &count) != CS_OK)
        return 0;

    for (uint64_t i = 0; i < count; i++)
    {
        char idx_str[12];
        char key[64] = "HW/PCI/";
        char *k = strncat(key, itoa(i, idx_str, 16), 56);  //keys are hex (SysReg/pci.c)

        uint64_t ecam = 0;
        if (registry_readkey_uint(k, "ECAM_ADDR", &ecam) != CS_OK || ecam == 0)
            continue;

        pci_config_t *d = (pci_config_t *)pci_map_ecam(ecam);
        for (int b = 0; b < 6; b++)
        {
            uint32_t v = d->bar[b];
            if (v & 0x1)
                continue;                           //I/O BAR
            bool b64 = ((v & 0x6) == 0x4);
            if (b64 && b + 1 >= 6)
                break;                              //malformed 64-bit BAR in last slot
            uint64_t base = v & 0xFFFFFFF0;
            if (b64)
                base |= ((uint64_t)d->bar[b + 1] << 32);
            if (base != 0 && base < ceiling)
            {
                bool ms = d->command.mem_space;
                d->command.mem_space = 0;
                uint64_t sz = pci_bar_getsize((volatile uint32_t *)d->bar, b);
                d->command.mem_space = ms;
                uint64_t end = base + sz;
                //Only BARs lying wholly within the sub-ceiling window raise the
                //top; a mis-sized / high-mapped BAR must not push placement out.
                if (sz != 0 && end <= ceiling && end > top)
                    top = end;
            }
            if (b64)
                b++;                                //64-bit BAR consumes two slots
        }
    }
    return top;
}

// Open the memory-forwarding window covering [base, base+size) on EVERY PCI-to-PCI
// bridge whose forwarded bus range [secondary, subordinate] contains the device's
// bus -- exactly the set of bridges the device's MMIO traverses from the root bus
// down. This transparently handles PCIe root ports and nested/cascaded bridges,
// not just a single level. The window is 1 MiB-granular (rounded outward), and an
// already-open bridge window is *extended* to cover ours rather than clobbered
// (so sibling devices behind the same bridge keep working). Each touched bridge
// also gets memory decode + bus-master enabled -- bus-master so the device's
// upstream MSI/MSI-X writes are forwarded too. Returns true if the device is on
// the root bus (nothing to do) or at least one forwarding bridge was programmed.
static inline bool pci_bridge_open_window(uint64_t dev_ecam, uint64_t base, uint64_t size)
{
    uint64_t mcfg_base = dev_ecam & ~0x0FFFFFFFULL;
    uint32_t dev_bus = (uint32_t)((dev_ecam - mcfg_base) >> 20);
    if (dev_bus == 0)
        return true;                                //on root bus: no bridge to open

    uint64_t want_lo = base & ~(PCI_BRIDGE_GRAN - 1);
    uint64_t want_hi = (base + size + PCI_BRIDGE_GRAN - 1) & ~(PCI_BRIDGE_GRAN - 1);

    uint64_t count = 0;
    if (registry_readkey_uint("HW/PCI", "COUNT", &count) != CS_OK)
        return false;

    bool opened = false;
    for (uint64_t i = 0; i < count; i++)
    {
        char idx_str[12];
        char key[64] = "HW/PCI/";
        char *k = strncat(key, itoa(i, idx_str, 16), 56);
        uint64_t ecam = 0;
        if (registry_readkey_uint(k, "ECAM_ADDR", &ecam) != CS_OK || ecam == 0)
            continue;

        volatile uint8_t *cfg = (volatile uint8_t *)pci_map_ecam(ecam);
        if ((cfg[0x0E] & 0x7F) != 1)                //not a type-1 (bridge) header
            continue;
        uint8_t sec = cfg[0x19];                    //secondary bus
        uint8_t sub = cfg[0x1A];                    //subordinate bus
        if (!(sec <= dev_bus && dev_bus <= sub))    //doesn't forward our bus
            continue;

        volatile uint16_t *membase  = (volatile uint16_t *)(cfg + 0x20);
        volatile uint16_t *memlimit = (volatile uint16_t *)(cfg + 0x22);
        volatile uint16_t *command  = (volatile uint16_t *)(cfg + 0x04);

        //Reg layout: bits[15:4] = addr[31:20]; the limit's low 20 bits read as F.
        uint64_t lo = want_lo, hi = want_hi;
        uint16_t ob = *membase, ol = *memlimit;
        uint64_t exist_lo = ((uint64_t)(ob & 0xFFF0)) << 16;
        uint64_t exist_hi = (((uint64_t)(ol & 0xFFF0)) << 16) + PCI_BRIDGE_GRAN;
        if (exist_lo < exist_hi)                    //existing window is open: union with ours
        {
            if (exist_lo < lo) lo = exist_lo;
            if (exist_hi > hi) hi = exist_hi;
        }

        *membase  = (uint16_t)((lo >> 16) & 0xFFF0);
        *memlimit = (uint16_t)(((hi - 1) >> 16) & 0xFFF0);
        *command  = (uint16_t)(*command | 0x6);     //memory decode | bus-master
        opened = true;
    }
    return opened;
}

// Assign MMIO addresses to a device's unassigned memory BARs and open the bridge
// path so they are reachable. Packs every unassigned memory BAR (size-aligned)
// into a free region just above the existing MMIO assignments, opens the
// forwarding window on every bridge between the device and the root bus, and
// enables the device's memory decode + bus-master. `ecam_phys` is the device's
// ECAM physical address (its bus is decoded from it). Returns the base of the
// first non-prefetchable memory BAR (the register window), or 0 on failure.
static inline uint64_t pci_assign_bars(pci_config_t *device, uint64_t ecam_phys)
{
    //Ceiling = base of this device's ECAM/MCFG window (256 MiB-aligned), which
    //bounds the host-bridge MMIO aperture we can safely place into.
    uint64_t ceiling = ecam_phys & ~0x0FFFFFFFULL;

    device->command.mem_space = 0;                  //disable decode while reprogramming

    uint64_t winbase = (pci_mmio_used_top(ceiling) + (PCI_BRIDGE_GRAN - 1)) & ~(PCI_BRIDGE_GRAN - 1);
    if (winbase == 0 || winbase >= ceiling)
    {
        device->command.mem_space = 1;
        return 0;
    }

    uint64_t reg_bar = 0;
    uint64_t cursor = winbase;
    for (int i = 0; i < 6; i++)
    {
        uint32_t b = device->bar[i];
        if (b & 0x1)
            continue;                               //I/O BAR
        if ((b & 0x0F) == 0)
            continue;                               //empty/unimplemented slot
        bool is64 = ((b & 0x6) == 0x4);
        uint32_t type_bits = b & 0x0F;
        uint64_t size = pci_bar_getsize((volatile uint32_t *)device->bar, i);
        if (size == 0)
        {
            if (is64) i++;
            continue;
        }
        uint64_t addr = (cursor + size - 1) & ~(size - 1);   //align to BAR size
        if (addr + size > ceiling)                  //out of the host-bridge aperture
        {
            device->command.mem_space = 1;
            return 0;
        }
        device->bar[i] = (uint32_t)(addr & 0xFFFFFFF0) | type_bits;
        if (is64)
            device->bar[i + 1] = (uint32_t)(addr >> 32);
        cursor = addr + size;
        if (reg_bar == 0 && !(b & 0x8))             //non-prefetchable -> register BAR
            reg_bar = addr;
        if (is64)
            i++;                                    //64-bit BAR consumes two slots
    }

    if (reg_bar == 0)
    {
        device->command.mem_space = 1;
        return 0;
    }

    //Open the forwarding window over the actual assigned extent on every bridge
    //on the path to the root bus.
    if (!pci_bridge_open_window(ecam_phys, winbase, cursor - winbase))
    {
        device->command.mem_space = 1;
        return 0;
    }

    device->command.mem_space = 1;
    device->command.busmaster = 1;
    return reg_bar;
}

#endif
