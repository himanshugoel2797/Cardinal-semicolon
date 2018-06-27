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

int module_init(void *ecam_addr){

    local_spinlock_lock(&device_init_lock);
    pci_config_t *device = (pci_config_t*)vmem_phystovirt((intptr_t)ecam_addr, KiB(4), vmem_flags_uncached | vmem_flags_kernel | vmem_flags_rw);

    //enable pci bus master
    device->command.busmaster = 1;

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

    uintptr_t corb_rirb_buffer_phys = pagealloc_alloc(0, 0, physmem_alloc_flags_data, (instance->corb.entcnt + instance->rirb.entcnt) * sizeof(uint32_t));

    uint32_t *corb_rirb_buffer = (uint32_t*)vmem_phystovirt(corb_rirb_buffer_phys, (instance->corb.entcnt + instance->rirb.entcnt) * sizeof(uint32_t), vmem_flags_uncached | vmem_flags_kernel | vmem_flags_rw);
    instance->corb.buffer = corb_rirb_buffer;
    instance->rirb.buffer = corb_rirb_buffer + instance->corb.entcnt;

    

    hdaudio_initialize(instance);

    DEBUG_PRINT("HD Audio Initialized\r\n");
    local_spinlock_unlock(&device_init_lock);

    return 0;
}

int hdaudio_setupbuffersz(hdaudio_instance_t *instance, bool corb) {
    uint32_t entcnt_cap = instance->cfg_regs->rirb->sz.szcap;
    if(corb) {
        entcnt_cap = instance->cfg_regs->corb->sz.szcap;
    }

    if(entcnt_cap & (1 << 2))
        entcnt_cap = 256;
    else if(entcnt_cap & (1 << 1))
        entcnt_cap = 16;
    else if(entcnt_cap & (1 << 0))
        entcnt_cap = 2;
    
    if(corb){
        instance->corb.entcnt = entcnt_cap;
    }else{
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

    if(corb){
        instance->cfg_regs->corb->entcnt = entcnt_cap;
    }else{
        instance->cfg_regs->rirb->entcnt = entcnt_cap;
    }
}

int hdaudio_initialize(hdaudio_instance_t *instance){

    //bring the device out of reset
    intance->gctl.crst = 1;
    while(instance->gctl.crst != 1)
        ;

    //
}