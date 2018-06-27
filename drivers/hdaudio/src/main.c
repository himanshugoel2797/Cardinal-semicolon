/**
 * Copyright (c) 2018 Himanshu Goel
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

#include "regs.h"
#include "hdaudio.h"

static int device_init_lock = 0;
static hdaudio_instance_t *instances = NULL;


int hdaudio_setupbuffersz(hdaudio_instance_t *instance, bool corb) {
    uint32_t entcnt_cap = instance->cfg_regs->rirb.sz.szcap;
    if(corb) {
        entcnt_cap = instance->cfg_regs->corb.sz.szcap;
    }

    if(entcnt_cap & (1 << 2))
        entcnt_cap = 256;
    else if(entcnt_cap & (1 << 1))
        entcnt_cap = 16;
    else if(entcnt_cap & (1 << 0))
        entcnt_cap = 2;

    if(corb) {
        instance->corb.entcnt = entcnt_cap;
    } else {
        instance->rirb.entcnt = entcnt_cap;
    }

    switch(entcnt_cap) {
    case 2:
        entcnt_cap = 0;
        break;
    case 16:
        entcnt_cap = 1;
        break;
    case 256:
        entcnt_cap = 2;
        break;
    }

    if(corb) {
        instance->cfg_regs->corb.sz.sz = entcnt_cap;
    } else {
        instance->cfg_regs->rirb.sz.sz = entcnt_cap;
    }

    return 0;
}

static void tmp_handler(int int_num) {
    int_num = 0;
    DEBUG_PRINT("HD Audio Interrupt\r\n");
}

int hdaudio_initialize(hdaudio_instance_t *instance) {

    //bring the device out of reset
    instance->cfg_regs->sdiwake = 0xFFFF;

    instance->cfg_regs->gctl.crst = 0;
    while(instance->cfg_regs->gctl.crst != 0)
        ;
    instance->cfg_regs->gctl.crst = 1;
    while(instance->cfg_regs->gctl.crst != 1)
        ;
    while(instance->cfg_regs->sdiwake == 0)
        ;
    DEBUG_PRINT("Codecs Enumerated!\r\n");
    instance->cfg_regs->gctl.crst |= (1 << 8);

    //Enable state change interrupts
    instance->cfg_regs->sdiwen = 0xFFFF;
    instance->cfg_regs->intctl.cie = 1;
    instance->cfg_regs->intctl.gie = 1;

    //Wait for the codecs to request state changes


    //Configure CORB
    instance->cfg_regs->corb.ctl.corbrun = 0;
    while(instance->cfg_regs->corb.ctl.corbrun)
        ;

    instance->cfg_regs->corb.lower_base = (uint32_t)instance->corb.buffer_phys;
    instance->cfg_regs->corb.upper_base = (uint32_t)(instance->corb.buffer_phys >> 32);

    {
        //Reset the CORB read pointer
        instance->cfg_regs->corb.rp = (1 << 15);
        while(!(instance->cfg_regs->corb.rp & (1 << 15)))
            ;
        instance->cfg_regs->corb.rp = 0;
        while(instance->cfg_regs->corb.rp & (1 << 15))
            ;
    }
    instance->cfg_regs->corb.wp = 0;

    //Start the corb again
    instance->cfg_regs->corb.ctl.corbrun = 1;
    while(!instance->cfg_regs->corb.ctl.corbrun)
        ;

    //Configure RIRB
    instance->cfg_regs->rirb.ctl.dmaen = 0;
    while(instance->cfg_regs->rirb.ctl.dmaen)
        ;

    instance->cfg_regs->rirb.lower_base = (uint32_t)instance->rirb.buffer_phys;
    instance->cfg_regs->rirb.upper_base = (uint32_t)(instance->rirb.buffer_phys >> 32);

    //Reset the RIRB write pointer
    instance->cfg_regs->rirb.wp |= (1 << 15);

    //Set the number of messages after which to interrupt
    instance->cfg_regs->rirb.intcnt = 1;

    //Start the rirb again
    instance->cfg_regs->rirb.ctl.dmaen = 1;
    while(!instance->cfg_regs->rirb.ctl.dmaen)
        ;

#define VERB_MSG(addr, node, payload) (addr << 28 | (node & 0xFF) << 20 | (payload & 0xFFFFF))

    //uint32_t *direct_buf = (uint32_t*)0xffff8000febd0060;

    instance->corb.buffer[1] = VERB_MSG(0, 0, 0xF0000);
    instance->cfg_regs->corb.wp++;
    //Send a test verb
    while(instance->cfg_regs->corb.rp == 0)
        ;

    DEBUG_PRINT("HD Audio Initialized\r\n");
    //Build the device graph


    return 0;
}

int module_init(void *ecam_addr) {

    local_spinlock_lock(&device_init_lock);
    pci_config_t *device = (pci_config_t*)vmem_phystovirt((intptr_t)ecam_addr, KiB(4), vmem_flags_uncached | vmem_flags_kernel | vmem_flags_rw);

    //enable pci bus master
    device->command.busmaster = 1;

    //interrupt setup
    int int_cnt = 0;
    int msi_val = pci_getmsiinfo(device, &int_cnt);

    int int_val = 0;
    interrupt_allocate(1, interrupt_flags_none, &int_val);
    interrupt_registerhandler(int_val, tmp_handler);

    uintptr_t msi_addr = (uintptr_t)msi_register_addr(0);
    uint32_t msi_msg = msi_register_data(int_val);
    pci_setmsiinfo(device, msi_val, &msi_addr, &msi_msg, 1);

    //figure out which bar to use
    uint64_t bar = 0;
    for(int i = 0; i < 6; i++) {
        if((device->bar[i] & 0x6) == 0x4) //Is 64-bit
            bar = (device->bar[i] & 0xFFFFFFF0) + ((uint64_t)device->bar[i + 1] << 32);
        else if((device->bar[i] & 0x6) == 0x0) //Is 32-bit
            bar = (device->bar[i] & 0xFFFFFFF0);
        if(bar) break;
    }

    //create the device entry
    hdaudio_instance_t *instance = (hdaudio_instance_t*)malloc(sizeof(hdaudio_instance_t));
    instance->cfg_regs = (hdaudio_regs_t*)vmem_phystovirt(bar, KiB(4), vmem_flags_uncached | vmem_flags_kernel | vmem_flags_rw);
    instance->next = NULL;

    if(instances == NULL)
        instances = instance;
    else {
        instance->next = instances;
        instances = instance;
    }

    //Figure out corb and rirb size and allocate memory
    hdaudio_setupbuffersz(instance, false);
    hdaudio_setupbuffersz(instance, true);

    uintptr_t corb_rirb_buffer_phys = pagealloc_alloc(0, 0, physmem_alloc_flags_data, (instance->corb.entcnt + 2 * instance->rirb.entcnt) * sizeof(uint32_t));

    uint32_t *corb_rirb_buffer = (uint32_t*)vmem_phystovirt(corb_rirb_buffer_phys, (instance->corb.entcnt + 2 * instance->rirb.entcnt) * sizeof(uint32_t), vmem_flags_uncached | vmem_flags_kernel | vmem_flags_rw);
    instance->corb.buffer = corb_rirb_buffer;
    instance->rirb.buffer = corb_rirb_buffer + instance->corb.entcnt;

    instance->corb.buffer_phys = corb_rirb_buffer_phys;
    instance->rirb.buffer_phys = corb_rirb_buffer_phys + instance->corb.entcnt * sizeof(uint32_t);

    hdaudio_initialize(instance);

    local_spinlock_unlock(&device_init_lock);

    return 0;
}