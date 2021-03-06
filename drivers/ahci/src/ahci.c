/**
 * Copyright (c) 2019 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <cardinal/local_spinlock.h>

#include "SysVirtualMemory/vmem.h"
#include "SysTaskMgr/task.h"
#include "SysTimer/timer.h"

#include "ahci.h"

PRIVATE void ahci_write8(ahci_instance_t *inst, uint32_t off, uint8_t val)
{
    *(uint8_t *)(inst->cfg8 + off) = val;
}

PRIVATE void ahci_write16(ahci_instance_t *inst, uint32_t off, uint16_t val)
{
    *(uint16_t *)(inst->cfg8 + off) = val;
}

PRIVATE void ahci_write32(ahci_instance_t *inst, uint32_t off, uint32_t val)
{
    *(uint32_t *)(inst->cfg8 + off) = val;
}

PRIVATE uint8_t ahci_read8(ahci_instance_t *inst, uint32_t off)
{
    return *(uint8_t *)(inst->cfg8 + off);
}

PRIVATE uint16_t ahci_read16(ahci_instance_t *inst, uint32_t off)
{
    return *(uint16_t *)(inst->cfg8 + off);
}

PRIVATE uint32_t ahci_read32(ahci_instance_t *inst, uint32_t off)
{
    return *(uint32_t *)(inst->cfg8 + off);
}

PRIVATE void ahci_resethba(ahci_instance_t *inst)
{
    //Start the reset
    ahci_write32(inst, HBA_GHC, 1);

    //Wait for the HBA to be done reseting
    while (ahci_read32(inst, HBA_GHC) & 1)
        ;
}

PRIVATE uint32_t ahci_readports(ahci_instance_t *inst)
{
    return ahci_read32(inst, HBA_PI);
}

PRIVATE void ahci_obtainownership(ahci_instance_t *inst)
{
    ahci_write32(inst, HBA_BOHC, (1 << 1)); //Obtain ownership of the HBA
    task_sleep(task_current(), 25 * 1000 * 1000);           //Sleep 25ms
    if (ahci_read32(inst, HBA_BOHC) & (1 << 4))
        task_sleep(task_current(), 2000 * 1000 * 1000); //wait 2 seconds for the BIOs to finish up
}

PRIVATE void ahci_reportawareness(ahci_instance_t *inst)
{
    ahci_write32(inst, HBA_GHC, (1 << 31));
}

PRIVATE int ahci_initializeport(ahci_instance_t *inst, int index)
{
    uint32_t cmd = ahci_read32(inst, HBA_PxCMD(index));

    uint32_t dma_base = (uint32_t)inst->port_dma.phys_addr;
    uintptr_t dma_vaddr = inst->port_dma.virt_addr;

    //Ensure the device is idle before starting initialization
    if ((cmd & (HBA_PxCMD_FRE | HBA_PxCMD_FR | HBA_PxCMD_CR | HBA_PxCMD_ST)) != 0)
    {
        //Attempt to clear the offending bits
        cmd &= ~(HBA_PxCMD_ST | HBA_PxCMD_FRE);

        for (int i = 0; i < 5; i++)
        {
            task_sleep(task_current(), 500 * 1000 * 1000); //Sleep 500ms
            if ((ahci_read32(inst, HBA_PxCMD(index)) & (HBA_PxCMD_FR | HBA_PxCMD_CR)) == 0)
                break;
        }
        //Failed to bring the device into an idle state, can't continue with init
        if ((ahci_read32(inst, HBA_PxCMD(index)) & (HBA_PxCMD_FR | HBA_PxCMD_CR)) != 0)
            return -1;
    }

    if (ahci_read32(inst, HBA_PxSACT(index)) != 0)
        PANIC("Expected 1!");

    //Command buffer is 1024 bytes
    ahci_write32(inst, HBA_PxCLB(index), dma_base + (index * CMD_BUF_SIZE));
    ahci_write32(inst, HBA_PxCLBU(index), 0);

    //FIS buffer is 256 bytes
    ahci_write32(inst, HBA_PxFB(index), dma_base + 32 * CMD_BUF_SIZE + (index * FIS_SIZE));
    ahci_write32(inst, HBA_PxFBU(index), 0);

    //Setup the first command table entry
    ahci_cmd_t *cmd_entry = (ahci_cmd_t *)(dma_vaddr + (index * CMD_BUF_SIZE));
    for (int i = 0; i < 32; i++)
    {
        cmd_entry->PRDTL = 64;
        cmd_entry->commandTableBaseAddress = (dma_base + 32 * (CMD_BUF_SIZE + FIS_SIZE) + index * sizeof(ahci_cmdtable_t));
        cmd_entry->commandTableBaseAddressUpper = 0;
        cmd_entry++;
    }

    //Enable receiving FIS's
    ahci_write32(inst, HBA_PxCMD(index), ahci_read32(inst, HBA_PxCMD(index)) | HBA_PxCMD_FRE);

    //Clear the SERR register by writing ones to all implemented bits
    ahci_write32(inst, HBA_PxSERR(index), 0x07FF0F03);

    uint32_t tfd = ahci_read32(inst, HBA_PxTFD(index));

    //Ensure that the device is functional before setting the bit in the active mask
    //and enabling the HBA

    if ((tfd & (HBA_PxTFD_BSY | HBA_PxTFD_DRQ)) != 0)
        return -2; //The device is not in functioning order, TODO we might want to report this

    uint32_t ssts = ahci_read32(inst, HBA_PxSSTS(index));

    if ((ssts & HBA_PxSSTS_DET_MASK) != 3)
        return -3; //TODO report this, by writing to a virtual log file or something

    //The device is functional, mark it as so and start it up
    local_spinlock_lock(&inst->lock);
    inst->activeDevices |= (1 << index);
    inst->activeCmdBits[index] = 0;
    inst->finishedCmdBits[index] = 0;
    local_spinlock_unlock(&inst->lock);

    ahci_write32(inst, HBA_PxCMD(index), ahci_read32(inst, HBA_PxCMD(index)) | HBA_PxCMD_ST);
    return 0;
}

PRIVATE int ahci_getcmdslot(ahci_instance_t *inst, int index)
{
    local_spinlock_lock(&inst->lock);
    uint32_t cmd_slots = inst->activeCmdBits[index];

    for (int i = 0; i < 32; i++)
        if (~cmd_slots & (1 << i))
        {
            local_spinlock_unlock(&inst->lock);
            return i;
        }

    local_spinlock_unlock(&inst->lock);
    return -1;
}

PRIVATE int ahci_readdev(ahci_instance_t *inst, int index, uint64_t loc, void *addr, uint32_t len)
{

    uint32_t dma_base = (uint32_t)inst->port_dma.phys_addr;
    uintptr_t dma_vaddr = inst->port_dma.virt_addr;

    uint32_t target_len = len;
    if (target_len % (32 * 1024))
        target_len -= target_len % (32 * 1024);

    //Clear interrupt flags
    ahci_write32(inst, HBA_PxIS(index), 0xFFFFFFFF);

    int slot = ahci_getcmdslot(inst, index);

    ahci_cmd_t *ahci_cmd = (ahci_cmd_t *)(dma_vaddr + (index * CMD_BUF_SIZE));
    ahci_cmd += slot;

    ahci_cmd->info.cfl = sizeof(register_h2d_fis_t) / sizeof(uint32_t);
    ahci_cmd->info.write = 0;
    ahci_cmd->info.atapi = 0;

    ahci_cmd->PRDTL = (target_len / (32 * 1024));

    ahci_cmdtable_t *cmd_table = (ahci_cmdtable_t *)(dma_vaddr + 32 * (CMD_BUF_SIZE + FIS_SIZE) + index * sizeof(ahci_cmdtable_t));

    uint64_t p_addr = 0;
    vmem_virttophys(NULL, (intptr_t)addr, (intptr_t *)&p_addr);
    uint64_t p_curaddr = p_addr;

    int i = 0;
    for (; i < 127 && target_len > 32 * 1024; i++)
    {
        cmd_table->prdt[i].baseAddress = (uint32_t)p_curaddr;
        cmd_table->prdt[i].baseAddressUpper = (uint32_t)(p_curaddr >> 32);
        cmd_table->prdt[i].rsv0 = 0;
        cmd_table->prdt[i].rsv1 = 0;
        cmd_table->prdt[i].byteCount = 32 * 1024;
        cmd_table->prdt[i].intCompletion = 0;

        target_len -= 32 * 1024;
        p_curaddr += 32 * 1024;
    }
    cmd_table->prdt[i].baseAddress = (uint32_t)p_curaddr;
    cmd_table->prdt[i].baseAddressUpper = (uint32_t)(p_curaddr >> 32);
    cmd_table->prdt[i].rsv0 = 0;
    cmd_table->prdt[i].rsv1 = 0;
    cmd_table->prdt[i].byteCount = target_len;
    cmd_table->prdt[i].intCompletion = 0;

    register_h2d_fis_t *fis = (register_h2d_fis_t *)cmd_table->cmd_fis;
    fis->fisType = fis_type_RegisterH2D;
    fis->cmd = 1;
    fis->command = ATA_CMD_READ_DMA_EXT;
    fis->lba_lo = (uint16_t)loc;
    fis->lba_mid = (uint8_t)(loc >> 16);
    fis->lba_mid_h = (uint16_t)(loc >> 24);
    fis->lba_hi = (uint8_t)(loc >> 40);

    fis->device = 1 << 6; //LBA48 mode
    fis->count = target_len / 512;

    ahci_write32(inst, HBA_PxSACT(index), 1 << slot);
    ahci_write32(inst, HBA_PxCI(index), 1 << slot);

    local_spinlock_lock(&inst->lock);
    inst->activeCmdBits[index] |= (1 << slot);
    local_spinlock_unlock(&inst->lock);

    while (ahci_read32(inst, HBA_PxCI(index)) & (1 << slot))
        DEBUG_PRINT("[AHCI] Read Pending\r\n");
    return 0;
}