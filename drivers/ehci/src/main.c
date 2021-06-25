/**
 * Copyright (c) 2021 himanshu
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
#include "SysTaskMgr/task.h"
#include "pci/pci.h"
#include "CoreUsb/usb.h"

#include "ehci.h"

static ehci_ctrl_state_t *instances = NULL;
static int instance_count = 0;
static int instance_lock = 0;

int module_init(void *ecam_addr)
{
    pci_config_t *device = (pci_config_t *)vmem_phystovirt((intptr_t)ecam_addr, KiB(4), vmem_flags_uncached | vmem_flags_kernel | vmem_flags_rw);

    //enable pci bus master
    device->command.busmaster = 1;

    //TODO: interrupts are not supported, so use a polling task
    ehci_ctrl_state_t *instance = malloc(sizeof(ehci_ctrl_state_t));

    int cli_state = cli();
    local_spinlock_lock(&instance_lock);
    instance->id = instance_count++;
    instance->next = instances;
    instances = instance;
    local_spinlock_unlock(&instance_lock);
    sti(cli_state);

    uint64_t bar = (device->bar[0] & 0xFFFFFFF0);
    instance->membar = bar;

    //Frame list: 4x1024 entries
    //Each frame contains transfer descriptors
    //Queue Heads are for bulk transfers
    instance->framelist_pmem = (uint32_t)pagealloc_alloc(0, 0, physmem_alloc_flags_32bit | physmem_alloc_flags_data | physmem_alloc_flags_zero, KiB(4));
    instance->framelist = (uint32_t *)vmem_phystovirt((intptr_t)instance->framelist_pmem, KiB(4), vmem_flags_uncached | vmem_flags_kernel | vmem_flags_rw);
    instance->init_complete = false;

    usb_hci_desc_t *desc = malloc(sizeof(usb_hci_desc_t));  //TODO: allocate on stack, CoreUsb should make own copy
    itoa(instance->id, desc->name, 10);
    desc->state = instance;
    desc->device_type = usb_device_type_uhci;
    desc->lock = 0;
    usb_register_hostcontroller(desc, &instance->handle);

    //Start polling process
    //cs_id int_task = 0;
    //create_task_kernel("ehci_int_poll", task_permissions_kernel, &int_task);
    //start_task_kernel(int_task, (void (*)(void *))intr_handler, instance);
    //instance->intr_task = int_task;

    //Reset HCI
    //uhci_reset(instance);

    //Enable all ports
    //for (int i = 0; i < PORT_COUNT; i++)
    //    uhci_enableport(instance, i);
    instance->init_complete = true;

    DEBUG_PRINT("[EHCI] Init complete\r\n");

    return 0;
}