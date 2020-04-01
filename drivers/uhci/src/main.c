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

#include "uhci.h"

static uhci_ctrl_state_t *instances = NULL;
static int instance_lock = 0;

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

    return 0;
}