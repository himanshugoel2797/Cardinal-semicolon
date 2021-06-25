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
#include "SysTaskMgr/task.h"
#include "pci/pci.h"
#include "CoreUsb/usb.h"

#include "uhci.h"

static uhci_ctrl_state_t *instances = NULL;
static int instance_count = 0;
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
    write16(state, USBCMD_REG, USBCMD_GRESET);
    task_sleep(task_current(), 10 * 1000 * 1000);
    write16(state, USBCMD_REG, 0);  //Exit GRESET 10ms after starting

    //now perform an HCRESET
    write16(state, USBCMD_REG, USBCMD_HCRESET);
    while(read16(state, USBCMD_REG) & USBCMD_HCRESET)   //wait for HCRESET to finish
        ;
    write32(state, FRBASEADDR_REG, state->framelist_pmem);
    write16(state, USBCMD_REG, 1);
}

static void uhci_enableport(uhci_ctrl_state_t *state, int idx)
{
    uint16_t v = read16(state, PORTSCn_REG(idx));
    
    //Reset port
    write16(state, PORTSCn_REG(idx), PORTSC_PORTRESET);
    task_sleep(task_current(), 10 * 1000 * 1000);
    write16(state, PORTSCn_REG(idx), 0);
    task_sleep(task_current(), 500 * 1000 * 1000);
    write16(state, PORTSCn_REG(idx), PORTSC_PORTEN | PORTSC_PORTENCHG);
}

static void intr_handler(uhci_ctrl_state_t *inst){
    while (!inst->init_complete)
        ;
    while (true)
    {
        uint16_t sts = read16(inst, USBSTS_REG);

        for (int i = 0; i < PORT_COUNT; i++){
            uint16_t p_sts = read16(inst, PORTSCn_REG(i));

            if (p_sts & PORTSC_CONNECTCHG){
                if (p_sts & PORTSC_CURCONNECT){
                    DEBUG_PRINT("[UHCI] Device connected\r\n");
                }else{
                    DEBUG_PRINT("[UHCI] Device disconnected\r\n");
                }
                //usb_device_connection_changed(inst->handle, i, !!(p_sts & PORTSC_CURCONNECT));
                write16(inst, PORTSCn_REG(i), PORTSC_CONNECTCHG);   //Acknowledge connection status change
            }
        }

        if (sts & USBSTS_USBINT)
        {
            DEBUG_PRINT("[UHCI] USB Interrupt\r\n");
            write16(inst, USBSTS_REG, USBSTS_USBINT); //clear the interrupt
            continue;
        }
        task_yield(); //halt(); //swap for yield later
    }
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
    instance->id = instance_count++;
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
    instance->framelist = (uhci_framelist_entry_t *)vmem_phystovirt((intptr_t)instance->framelist_pmem, KiB(4), vmem_flags_uncached | vmem_flags_kernel | vmem_flags_rw);
    instance->init_complete = false;

    //All frames are initially invalid
    for (int i = 0; i < FRAME_COUNT; i++)
        instance->framelist[i].invalid = 1;

    usb_hci_desc_t *desc = malloc(sizeof(usb_hci_desc_t));
    itoa(instance->id, desc->name, 10);
    desc->state = instance;
    desc->device_type = usb_device_type_uhci;
    desc->lock = 0;
    usb_register_hostcontroller(desc, &instance->handle);

    //Start polling process
    cs_id int_task = 0;
    create_task_kernel("uhci_int_poll", task_permissions_kernel, &int_task);
    start_task_kernel(int_task, (void (*)(void *))intr_handler, instance);
    instance->intr_task = int_task;

    //Reset HCI
    uhci_reset(instance);

    //Enable all ports
    for (int i = 0; i < PORT_COUNT; i++)
        uhci_enableport(instance, i);
    instance->init_complete = true;

    DEBUG_PRINT("[UHCI] Init complete\r\n");

    return 0;
}