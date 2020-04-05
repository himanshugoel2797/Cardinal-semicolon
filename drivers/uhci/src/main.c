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
#include "SysTimer/timer.h"
#include "pci/pci.h"

#include "uhci.h"

static uhci_ctrl_state_t *instances = NULL;
static int instance_lock = 0;

static void write8(uhci_ctrl_state_t *state, uint16_t addr, uint8_t val)
{
    outb(state->iobar + addr, val);
}

static void write16(uhci_ctrl_state_t *state, uint16_t addr, uint16_t val)
{
    outw(state->iobar + addr, val);
}

static void write32(uhci_ctrl_state_t *state, uint16_t addr, uint32_t val)
{
    outl(state->iobar + addr, val);
}

static uint8_t read8(uhci_ctrl_state_t *state, uint16_t addr)
{
    return inb(state->iobar + addr);
}

static uint16_t read16(uhci_ctrl_state_t *state, uint16_t addr)
{
    return inw(state->iobar + addr);
}

static uint32_t read32(uhci_ctrl_state_t *state, uint16_t addr)
{
    return inl(state->iobar + addr);
}

static void uhci_reset(uhci_ctrl_state_t *state)
{
    write16(state, USBCMD_REG, 1 << 2);
    timer_wait(10 * 1000 * 1000);
    write32(state, FRBASEADDR_REG, state->framelist_pmem);
    write16(state, USBCMD_REG, 1);
}

static void uhci_enableport(uhci_ctrl_state_t *state, int idx)
{
    uint16_t v = read16(state, PORTSCn_REG(idx));
    v = (v & ~(1 << 9));  //Bring port out of reset
    v = (v & ~(1 << 12)); //Bring port out of suspend
    v = (v | (1 << 2));   //Enable port
    write16(state, PORTSCn_REG(idx), v);
}

int module_init(void *ecam_addr)
{
    pci_config_t *device = (pci_config_t *)vmem_phystovirt((intptr_t)ecam_addr, KiB(4), vmem_flags_uncached | vmem_flags_kernel | vmem_flags_rw);

    //enable pci bus master
    device->command.busmaster = 1;

    //TODO: interrupts are not supported, so use a polling task
    uhci_ctrl_state_t *instance = malloc(sizeof(uhci_ctrl_state_t));

    int cli_state = cli();
    local_spinlock_lock(&instance_lock);
    instance->next = instances;
    instances = instance;
    local_spinlock_unlock(&instance_lock);
    sti(cli_state);

    uint64_t bar = (device->bar[4] & 0xFFFFFFF0); //I/O space BAR
    instance->iobar = bar;

    //Frame list: 4x1024 entries
    //Each frame contains transfer descriptors
    //Queue Heads are for bulk transfers
    instance->framelist_pmem = (uint32_t)pagealloc_alloc(0, 0, physmem_alloc_flags_32bit | physmem_alloc_flags_data | physmem_alloc_flags_zero, KiB(4));
    instance->framelist = (uint32_t *)vmem_phystovirt((intptr_t)instance->framelist_pmem, KiB(4), vmem_flags_uncached | vmem_flags_kernel | vmem_flags_rw);

    //Reset HCI
    uhci_reset(instance);

    //Start polling process

    //Enable all ports
    uhci_enableport(instance, 0);
    uhci_enableport(instance, 1);

    return 0;
}