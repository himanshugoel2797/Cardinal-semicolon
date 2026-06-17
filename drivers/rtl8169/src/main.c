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
#include "SysReg/registry.h"
#include "SysTimer/timer.h"
#include "pci/pci_irq.h"
#include "SysTaskMgr/task.h"

#include "state.h"

//Map a device's ECAM config space (4KiB) given its physical base.
static pci_config_t *map_ecam(uint64_t ecam_phys)
{
    return (pci_config_t *)vmem_phystovirt((intptr_t)ecam_phys, KiB(4),
            vmem_flags_uncached | vmem_flags_kernel | vmem_flags_rw);
}

//Bring the device to PCI power state D0. A RealTek NIC that firmware never
//initialised typically sits in D3hot: its config space is readable (so we see
//deviceID 0x8168) but its memory BARs are dead -- they read back 0 and won't
//even size. Walk the capability list to the Power-Management capability
//(ID 0x01) and clear the PowerState bits in PMCSR (cap+4). Spec requires up to
//10ms to settle after a D3->D0 transition before the device is usable.
static void power_on_d0(pci_config_t *device)
{
    uint8_t ptr = device->capabilitiesPtr;
    int guard = 0;
    while (ptr != 0 && guard++ < 48)
    {
        pci_cap_header_t *cap = (pci_cap_header_t *)((uint8_t *)device + ptr);
        if (cap->capID == pci_cap_pwm)
        {
            volatile uint16_t *pmcsr = (volatile uint16_t *)((uint8_t *)device + ptr + 4);
            uint16_t v = *pmcsr;
            if (v & 0x3)        //not already in D0
            {
                DEBUG_PRINT("[RTL8169] waking device D3->D0\r\n");
                *pmcsr = (uint16_t)(v & ~0x3);
                timer_wait(MS(10));
            }
            return;
        }
        ptr = cap->nextPtr;
    }
}

//Some firmware (e.g. the AtomicPi UEFI) never assigns BARs to devices it does
//not use for boot; they read back as bare type bits with a zero base, so
//pci_first_mmio_bar() returns 0 and the device is unreachable.
//Cardinal has no PCI resource allocator, so the helpers below are the first
//BAR-programming path in the tree. They are deliberately minimal and only
//handle a single small (<4GiB) memory BAR, which is all the RTL8168 register
//window (BAR2) needs.

//Size one BAR slot in-place: write all-ones, read back the decode mask, then
//restore the original value. Returns the BAR size in bytes (0 if the slot is
//unimplemented). The caller must have memory decode disabled on this device.
static uint64_t bar_size(volatile uint32_t *bars, int i)
{
    uint32_t orig = bars[i];
    bars[i] = 0xFFFFFFFF;
    uint32_t probe = bars[i];
    bars[i] = orig;
    uint32_t mask = probe & 0xFFFFFFF0;
    if (mask == 0)
        return 0;                       //unimplemented
    return (uint64_t)(~mask) + 1;       //low dword suffices for sub-4GiB BARs
}

//Highest end address (base+size) of any *assigned* memory BAR below `ceiling`,
//found by scanning every enumerated PCI device and sizing its BARs. This runs
//during driver load, before task_release_aps, so no AP/poll task is touching
//these devices concurrently -- briefly toggling a neighbour's memory-decode to
//size its BARs is safe here. Used to place our new BAR just above the
//firmware-assigned region (guaranteed inside the host-bridge MMIO aperture).
static uint64_t mmio_used_top(uint64_t ceiling)
{
    uint64_t count = 0, top = 0;
    if (registry_readkey_uint("HW/PCI", "COUNT", &count) != CS_OK)
        return 0;

    for (uint64_t i = 0; i < count; i++)
    {
        char idx_str[12];
        char key[64] = "HW/PCI/";
        char *k = strncat(key, itoa(i, idx_str, 10), 63);

        uint64_t ecam = 0;
        if (registry_readkey_uint(k, "ECAM_ADDR", &ecam) != CS_OK || ecam == 0)
            continue;

        pci_config_t *d = map_ecam(ecam);
        for (int b = 0; b < 6; b++)
        {
            uint32_t v = d->bar[b];
            if (v & 0x1)
                continue;                       //I/O BAR
            bool b64 = ((v & 0x6) == 0x4);
            uint64_t base = v & 0xFFFFFFF0;
            if (b64)
                base |= ((uint64_t)d->bar[b + 1] << 32);
            if (base != 0 && base < ceiling)
            {
                bool ms = d->command.mem_space;
                d->command.mem_space = 0;
                uint64_t sz = bar_size((volatile uint32_t *)d->bar, b);
                d->command.mem_space = ms;
                uint64_t end = base + sz;
                //Only let BARs that lie wholly within the sub-4GiB window below
                //the ceiling raise the top; a mis-sized or high-mapped BAR must
                //not push the placement out of the usable aperture.
                if (sz != 0 && end <= ceiling && end > top)
                    top = end;
            }
            if (b64)
                b++;                            //64-bit BAR consumes two slots
        }
    }
    return top;
}

//The NIC sits on a secondary PCI bus behind a root-port bridge (its ECAM offset
//from the MCFG base decodes to bus>0). If firmware assigned it no BARs, that
//bridge's memory-forwarding window is left closed, so NO MMIO address reaches
//the NIC (register reads return 0xff) regardless of what we program into the
//NIC's own BAR. Find the bridge whose secondary bus == the NIC's bus and open
//its non-prefetchable memory window (1MiB-granular) over the 1MiB region at
//`base`, and enable the bridge's memory decode + bus-mastering. The NIC is the
//lone device on that bus, so overwriting the window is safe. Returns true if a
//bridge was found and programmed.
static bool open_bridge_window(uint64_t nic_ecam, uint64_t base)
{
    uint64_t mcfg_base = nic_ecam & ~0x0FFFFFFFULL;
    uint32_t nic_bus = (uint32_t)((nic_ecam - mcfg_base) >> 20);
    if (nic_bus == 0)
        return true;                            //on bus 0: no bridge to open

    uint64_t count = 0;
    if (registry_readkey_uint("HW/PCI", "COUNT", &count) != CS_OK)
        return false;

    for (uint64_t i = 0; i < count; i++)
    {
        char idx_str[12];
        char key[64] = "HW/PCI/";
        char *k = strncat(key, itoa(i, idx_str, 10), 63);
        uint64_t ecam = 0;
        if (registry_readkey_uint(k, "ECAM_ADDR", &ecam) != CS_OK || ecam == 0)
            continue;

        volatile uint8_t *cfg = (volatile uint8_t *)map_ecam(ecam);
        if ((cfg[0x0E] & 0x7F) != 1)            //not a type-1 (bridge) header
            continue;
        if (cfg[0x19] != nic_bus)               //secondary bus != NIC's bus
            continue;

        //1MiB-granular non-prefetchable memory window covering [base, base+1MiB).
        //Reg layout: bits[15:4] = address[31:20]; limit's low 20 bits read as F.
        uint16_t mb = (uint16_t)((base >> 16) & 0xFFF0);
        uint16_t ml = (uint16_t)(((base + 0x100000 - 1) >> 16) & 0xFFF0);
        volatile uint16_t *membase  = (volatile uint16_t *)(cfg + 0x20);
        volatile uint16_t *memlimit = (volatile uint16_t *)(cfg + 0x22);
        volatile uint16_t *command  = (volatile uint16_t *)(cfg + 0x04);
        uint16_t old_mb = *membase, old_ml = *memlimit;
        *membase = mb;
        *memlimit = ml;
        *command = (uint16_t)(*command | 0x6);  //memory decode | bus-master

        {
            char t[20];
            DEBUG_PRINT("[RTL8169] bridge dev");
            DEBUG_PRINT(itoa((int)i, t, 10));
            DEBUG_PRINT(" sec_bus=");
            DEBUG_PRINT(itoa(cfg[0x19], t, 10));
            DEBUG_PRINT(" memwin old[0x");
            DEBUG_PRINT(itoa(old_mb, t, 16));
            DEBUG_PRINT(",0x");
            DEBUG_PRINT(itoa(old_ml, t, 16));
            DEBUG_PRINT("] new[0x");
            DEBUG_PRINT(itoa(mb, t, 16));
            DEBUG_PRINT(",0x");
            DEBUG_PRINT(itoa(ml, t, 16));
            DEBUG_PRINT("]\r\n");
        }
        return true;
    }
    DEBUG_PRINT("[RTL8169] no bridge found for NIC bus\r\n");
    return false;
}

//Assign the NIC's register BAR when firmware left it unprogrammed: pick the
//first implemented, non-prefetchable memory BAR (= BAR2, the register window;
//skip I/O BARs, unimplemented empty slots, and the prefetchable MSI-X BAR),
//size it, place it just above the highest firmware-assigned MMIO BAR, program
//it, and enable decode. Returns the assigned 64-bit physical base, or 0.
static uint64_t assign_mmio_bar(pci_config_t *device, uint64_t ecam_phys)
{
    //Ceiling = base of the ECAM/MCFG window (covers up to 256 buses = 256MiB
    //from a 256MiB-aligned base), recovered by aligning the device's ECAM down.
    uint64_t ceiling = ecam_phys & ~0x0FFFFFFFULL;

    //Disable our memory decode while sizing/reprogramming.
    device->command.mem_space = 0;

    //Select the register BAR by its TYPE BITS, not by sizing: an unassigned but
    //real memory BAR still carries its type bits (BAR2 reads 0x4 = 64-bit
    //memory), while a truly empty/unimplemented slot reads 0x0 (e.g. bar[1]
    //between the I/O BAR0 and the 64-bit BAR2). Skip I/O BARs, empty slots, and
    //the prefetchable MSI-X BAR (bit 3) -- what remains is the register window.
    int idx = -1;
    bool is64 = false;
    uint32_t type_bits = 0;
    for (int i = 0; i < 6; i++)
    {
        uint32_t b = device->bar[i];
        if (b & 0x1)
            continue;                           //I/O BAR
        if ((b & 0x0F) == 0)
            continue;                           //empty/unimplemented slot
        bool cand64 = ((b & 0x6) == 0x4);
        if (b & 0x8)                            //prefetchable -> MSI-X, not registers
        {
            if (cand64)
                i++;
            continue;
        }
        idx = i;
        is64 = cand64;
        type_bits = b & 0x0F;
        break;
    }
    if (idx < 0)
    {
        DEBUG_PRINT("[RTL8169] assign: no memory BAR (bars");
        for (int i = 0; i < 6; i++)
        {
            char t[12];
            DEBUG_PRINT(" ");
            DEBUG_PRINT(itoa((int)device->bar[i], t, 16));
        }
        DEBUG_PRINT(")\r\n");
        device->command.mem_space = 1;
        return 0;
    }

    //Size the chosen BAR; if it won't report a size, fall back to 64KiB, which
    //comfortably covers the RTL8168 register file.
    uint64_t size = bar_size((volatile uint32_t *)device->bar, idx);
    if (size == 0)
        size = 0x10000;

    //Place just above the highest assigned neighbour BAR, 1MiB-aligned (the
    //bridge memory window below is 1MiB-granular, so a clean 1MiB slot keeps the
    //window and the BAR aligned together). If that is unusable (no neighbours,
    //or it would reach the ceiling), fall back to a fixed address well inside
    //the Cherry Trail PCIe MMIO window and clear of the observed used region.
    uint64_t top = mmio_used_top(ceiling);
    uint64_t base = (top + 0xFFFFF) & ~0xFFFFFULL;
    bool fallback = false;
    if (base == 0 || base + 0x100000 > ceiling)
    {
        base = 0xd0000000ULL;
        fallback = true;
    }

    if (base == 0 || base + size > ceiling)     //even the fallback is unusable
    {
        DEBUG_PRINT("[RTL8169] self-assign: no valid placement\r\n");
        device->command.mem_space = 1;
        return 0;
    }

    //Open the upstream bridge's memory window FIRST so the address actually
    //reaches the NIC's bus, then program the NIC's BAR.
    open_bridge_window(ecam_phys, base);

    device->bar[idx] = (uint32_t)(base & 0xFFFFFFF0) | type_bits;
    if (is64)
        device->bar[idx + 1] = (uint32_t)(base >> 32);

    device->command.mem_space = 1;
    device->command.busmaster = 1;

    {
        char t[20];
        DEBUG_PRINT("[RTL8169] self-assigned BAR");
        DEBUG_PRINT(itoa(idx, t, 10));
        DEBUG_PRINT(" @ 0x");
        DEBUG_PRINT(ltoa((long long)base, t, 16));
        DEBUG_PRINT(fallback ? " (fallback, used_top 0x" : " (above used_top 0x");
        DEBUG_PRINT(ltoa((long long)top, t, 16));
        DEBUG_PRINT("), decode enabled\r\n");
    }
    return base;
}

int module_init(void *ecam_addr)
{
    pci_config_t *device = (pci_config_t *)vmem_phystovirt((intptr_t)ecam_addr, KiB(4), vmem_flags_uncached | vmem_flags_kernel | vmem_flags_rw);

    //Enable PCI memory-space decode AND bus-mastering BEFORE any BAR/MMIO
    //access. The chip reset is itself an MMIO write; with memory-space decode
    //off every register read returns 0xFF, so the reset bit appears never to
    //clear and bring-up aborts ("Reset timed out"). MSI worked regardless
    //because MSI is configured through config space, not the BAR. Both BSD
    //drivers set these two command bits before the first CSR access (FreeBSD
    //pci_enable_busmaster + RF_ACTIVE BAR map; OpenBSD pci_mapreg_map).
    device->command.mem_space = 1;
    device->command.busmaster = 1;

    //Bring the NIC to D0 before touching its registers. If firmware left it in
    //D3hot the memory BARs are dead (read/size as 0) until this transition.
    power_on_d0(device);

    //interrupt setup
    int int_val = pci_setup_msi(device, interrupt_flags_none);

    if (int_val < 0)
        DEBUG_PRINT("[RTL8169] NO MSI\r\n");

    {
        DEBUG_PRINT("[RTL8169] Allocated Interrupt Vector: ");
        char tmpbuf[10];
        DEBUG_PRINT(itoa(int_val, tmpbuf, 10));
        DEBUG_PRINT("\r\n");
    }

    //Find the MMIO register BAR robustly. The old hand-rolled
    //`bar[2] & 0xFFFFFFF0` only took the low 32 bits of what is a 64-bit memory
    //BAR on the 8168/8111 family: when UEFI maps that BAR above 4GB the low
    //half reads 0 and the real address lives in the next BAR slot, so the
    //driver mapped physical 0 and the reset "never cleared". pci_first_mmio_bar
    //(libs/pci) walks the BAR list, recombines 64-bit halves, and skips I/O and
    //empty BARs — the same helper the other drivers use.
    uint64_t bar = pci_first_mmio_bar(device);

    {
        char tmpbuf[20];
        DEBUG_PRINT("[RTL8169] deviceID=0x");
        DEBUG_PRINT(itoa(device->deviceID, tmpbuf, 16));
        DEBUG_PRINT(" mmio bar=0x");
        DEBUG_PRINT(ltoa((long long)bar, tmpbuf, 16));
        DEBUG_PRINT("\r\n");
    }

    //If firmware left the BARs unassigned (all read back as bare type bits),
    //assign the register BAR ourselves before touching the chip.
    if (bar == 0)
    {
        DEBUG_PRINT("[RTL8169] BARs unassigned by firmware; self-assigning.\r\n");
        bar = assign_mmio_bar(device, (uint64_t)ecam_addr);
        if (bar == 0)
        {
            //Give up on the NIC, but return success: a missing optional NIC must
            //not panic the whole system (CoreDriver PANICs on nonzero init).
            DEBUG_PRINT("[RTL8169] BAR self-assignment failed; NIC unavailable.\r\n");
            return 0;
        }
    }

    rtl8169_state_t *n_state = malloc(sizeof(rtl8169_state_t));
    memset(n_state, 0, sizeof(rtl8169_state_t));

    n_state->memar = (uint8_t *)vmem_phystovirt((intptr_t)bar, KiB(4), vmem_flags_uncached | vmem_flags_kernel | vmem_flags_rw);

    rtl8169_init(n_state);

    cs_id rtl_task = 0;
    task_create_kernel("rtl8169_int_poll", task_permissions_kernel, &rtl_task);
    task_start_kernel(rtl_task, (void (*)(void *))rtl8169_intr_handler, n_state);
    
    interrupt_register_handler(int_val, rtl8169_intr_routine);

    return 0;
}