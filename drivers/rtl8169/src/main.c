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
#include "pci/pci_alloc.h"
#include "SysTaskMgr/task.h"

#include "state.h"

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

    //NB: MSI-X is set up LATER, after rtl8169_init. Its table lives in the NIC's
    //memory (BAR4), and the chip's software reset (CR.RST at the top of
    //rtl8169_init) clears that table, so programming it before the reset would
    //lose it. The BAR *addresses* are assigned here (and survive the reset, like
    //config space does); only the table contents are written post-reset.

    //Find the MMIO register BAR. pci_first_mmio_bar walks the BAR list,
    //recombines 64-bit halves, and skips I/O/empty BARs (the same helper the
    //other drivers use). On the 8168/8111 the register window is the 64-bit BAR2.
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
    //assign them ourselves and open the bridge path -- pci_assign_bars (libs/pci)
    //places every unassigned memory BAR (the register window AND the
    //MSI-X-table BAR) above the firmware-assigned region and opens the
    //forwarding window on every bridge between the NIC and the root bus.
    if (bar == 0)
    {
        DEBUG_PRINT("[RTL8169] BARs unassigned by firmware; self-assigning.\r\n");
        bar = pci_assign_bars(device, (uint64_t)ecam_addr);
        if (bar == 0)
        {
            //Give up on the NIC, but return success: a missing optional NIC must
            //not panic the whole system (CoreDriver PANICs on nonzero init).
            DEBUG_PRINT("[RTL8169] BAR self-assignment failed; NIC unavailable.\r\n");
            return 0;
        }
        {
            char t[20];
            DEBUG_PRINT("[RTL8169] self-assigned register BAR @ 0x");
            DEBUG_PRINT(ltoa((long long)bar, t, 16));
            DEBUG_PRINT("\r\n");
        }
    }

    rtl8169_state_t *n_state = malloc(sizeof(rtl8169_state_t));
    memset(n_state, 0, sizeof(rtl8169_state_t));

    n_state->memar = (uint8_t *)vmem_phystovirt((intptr_t)bar, KiB(4), vmem_flags_uncached | vmem_flags_kernel | vmem_flags_rw);

    //If the chip won't initialise (reset timeout / OOM) don't start the poll
    //task or register the interrupt against a half-built state -- just leave the
    //NIC unavailable (return success so CoreDriver doesn't PANIC).
    if (rtl8169_init(n_state) != 0)
    {
        free(n_state);
        return 0;
    }

    //Set up MSI-X now, AFTER the reset (its table lives in BAR4 memory and is
    //cleared by the reset). pci_setup_msi_handler allocates the vector, registers
    //the handler, then enables the MSI-X capability -- in that order, so a first
    //message generated the instant the cap turns on can't be lost to an
    //unregistered vector (which would wedge edge-triggered MSI-X for good).
    int int_val = pci_setup_msi_handler(device, interrupt_flags_none, rtl8169_intr_routine);
    if (int_val < 0)
    {
        DEBUG_PRINT("[RTL8169] no MSI/MSI-X; NIC unavailable.\r\n");
        free(n_state);
        return 0;
    }
    {
        char tmpbuf[10];
        DEBUG_PRINT("[RTL8169] interrupt vector: ");
        DEBUG_PRINT(itoa(int_val, tmpbuf, 10));
        DEBUG_PRINT("\r\n");
    }

    cs_id rtl_task = 0;
    task_create_kernel("rtl8169_int_poll", task_permissions_kernel, &rtl_task);
    task_start_kernel(rtl_task, (void (*)(void *))rtl8169_intr_handler, n_state);

    //Enable the NIC's interrupt generation LAST -- the MSI-X cap is on, the
    //handler is registered, and the poll task is running. Enabling the device IMR
    //earlier would let the NIC raise an edge-triggered MSI-X for an already-
    //pending RX before the poll task exists; that message is lost and the
    //interrupt never re-arms.
    rtl8169_enable_interrupts(n_state);

    return 0;
}
