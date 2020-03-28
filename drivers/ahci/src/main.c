/**
 * Copyright (c) 2018 hgoel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */
#include <types.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>

#include <cardinal/local_spinlock.h>

#include "SysVirtualMemory/vmem.h"
#include "SysPhysicalMemory/phys_mem.h"
#include "SysInterrupts/interrupts.h"
#include "pci/pci.h"

#include "ahci.h"

static int device_init_lock = 0;
static ahci_instance_t *instance = NULL;
static uint32_t device_count = 0;

static void tmp_handler(int int_num)
{
    //Handle appropriately
    ahci_instance_t *iter = instance;
    while (iter != NULL)
    {
        if (iter->interrupt_vec == int_num)
        {
            //Check HBA_IS per port
            uint32_t g_is = ahci_read32(iter, HBA_IS);
            {
                char tmpbuf[10];
                DEBUG_PRINT("[AHCI] G_IS: ");
                DEBUG_PRINT(itoa(g_is, tmpbuf, 16));
                DEBUG_PRINT("\r\n");
            }
            for (int p_idx = 0; p_idx < 32; p_idx++)
                if (g_is & (1 << p_idx))
                {
                    //Check Px_IS to determine specific interrupt
                    uint32_t px_is = ahci_read32(iter, HBA_PxIS(p_idx));
                    if (px_is & HBA_PxIS_DHRS)
                    {
                        DEBUG_PRINT("[AHCI] D2H Register FIS\r\n");         //On receive
                        ahci_write32(iter, HBA_PxIS(p_idx), HBA_PxIS_DHRS); //Clear interrupt

                        uint32_t ci = ahci_read32(iter, HBA_PxCI(p_idx));

                        // ci's bits are always a subset of activeCmdBits
                        // thus, xor is bits where activeCmdBits is set, but ci is clear
                        local_spinlock_lock(&iter->lock);
                        uint32_t finishedCmds = iter->activeCmdBits[p_idx] ^ ci;
                        iter->finishedCmdBits[p_idx] |= finishedCmds;
                        iter->activeCmdBits[p_idx] = ci;
                        local_spinlock_unlock(&iter->lock);

                        //TODO: queue a notification to CoreStorage about finished tasks
                    }
                    ahci_write32(iter, HBA_IS, 1 << p_idx); //clear interrupt status
                }
        }
        iter = iter->next;
    }
}

int module_init(void *ecam_addr)
{
    local_spinlock_lock(&device_init_lock);
    pci_config_t *device = (pci_config_t *)vmem_phystovirt((intptr_t)ecam_addr, KiB(4), vmem_flags_uncached | vmem_flags_kernel | vmem_flags_rw);

    //enable pci bus master
    device->command.busmaster = 1;

    //interrupt setup
    int int_cnt = 0;
    int msi_val = pci_getmsiinfo(device, &int_cnt);

    if (msi_val < 0)
        DEBUG_PRINT("NO MSI\r\n");

    int int_val = 0;
    interrupt_allocate(1, interrupt_flags_none, &int_val);

    {
        DEBUG_PRINT("[AHCI] Allocated Interrupt Vector: ");
        char tmpbuf[10];
        DEBUG_PRINT(itoa(int_val, tmpbuf, 10));
        DEBUG_PRINT("\r\n");
    }

    uintptr_t msi_addr = (uintptr_t)msi_register_addr(0);
    uint32_t msi_msg = msi_register_data(int_val);
    pci_setmsiinfo(device, msi_val, &msi_addr, &msi_msg, 1);

    //figure out which bar to use
    uint64_t bar = 0;
    for (int i = 0; i < 6; i++)
    {
        if ((device->bar[i] & 0x7) == 0x4) //Is 64-bit memory mapped
            bar = (device->bar[i] & 0xFFFFFFF0) + ((uint64_t)device->bar[i + 1] << 32);
        else if ((device->bar[i] & 0x7) == 0x0) //Is 32-bit memory mapped
            bar = (device->bar[i] & 0xFFFFFFF0);
        if (bar)
            break;
    }

    instance = (ahci_instance_t *)malloc(sizeof(ahci_instance_t));
    instance->lock = 0;
    local_spinlock_lock(&instance->lock);
    instance->cfg = (uintptr_t)vmem_phystovirt(bar, KiB(4), vmem_flags_uncached | vmem_flags_kernel | vmem_flags_rw);
    instance->interrupt_vec = int_val;
    instance->next = NULL;
    device_count++;
    local_spinlock_unlock(&instance->lock);

    ahci_obtainownership(instance);
    ahci_reportawareness(instance);
    ahci_resethba(instance);
    ahci_reportawareness(instance);

    //Get implemented ports
    uint32_t ports = ahci_readports(instance);
    uint32_t port_cnt = 32; //popcnt32(ports);
    instance->implPortCnt = port_cnt;

    //allocate dma memory
    //32 * (CMD_BUF_SIZE + FIS_SIZE + sizeof(ahci_cmdtable_t))
    size_t dma_sz = port_cnt * (CMD_BUF_SIZE + FIS_SIZE + sizeof(ahci_cmdtable_t));
    instance->port_dma.phys_addr = (uint64_t)pagealloc_alloc(0, 0, physmem_alloc_flags_zero | physmem_alloc_flags_data, dma_sz);
    instance->port_dma.virt_addr = (uint64_t)vmem_vmalloc(dma_sz);

    vmem_map(NULL, (intptr_t)instance->port_dma.virt_addr, (intptr_t)instance->port_dma.phys_addr, dma_sz, vmem_flags_uncached | vmem_flags_kernel | vmem_flags_rw, 0);

    //initialize ports
    //register each active port to IO interface
    for (int i = 0; i < 32; i++)
        if (ports & (1 << i))
            if (ahci_initializeport(instance, i) == 0)
            {
                //Configure interrupts for the port
                ahci_write32(instance, HBA_PxIS(i), 0);
                ahci_write32(instance, HBA_IS, 0);

                //Descriptor Processed Interrupt
                //DMA Setup FIS Interrupt -
                //Device to Host Register FIS Interrupt - Status/Error notification
                ahci_write32(instance, HBA_PxIE(i), HBA_PxIS_DHRS);

                //Register to IO interface
            }
    //Exit init state
    local_spinlock_unlock(&device_init_lock);

    //Enable interrupts
    interrupt_registerhandler(int_val, tmp_handler);
    ahci_write32(instance, HBA_GHC, ahci_read32(instance, HBA_GHC) | (1 << 1));

    {
        uint64_t paddr = (uint64_t)pagealloc_alloc(0, 0, physmem_alloc_flags_zero | physmem_alloc_flags_data, KiB(32));
        uint64_t vaddr = (uint64_t)vmem_vmalloc(KiB(32));

        vmem_map(NULL, (intptr_t)vaddr, (intptr_t)paddr, KiB(32), vmem_flags_uncached | vmem_flags_kernel | vmem_flags_rw, 0);
        ahci_readdev(instance, 0, 0, (void *)vaddr, KiB(32));
        __asm__("cli\n\thlt" ::"a"(vaddr));
    }

    return 0;
}