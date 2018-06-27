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
#include "cmds.h"
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
    hdaudio_instance_t *instance = instances;
    while (instance->interrupt_vec != int_num){
        instance = instance->next;

        if(instance == NULL)
            PANIC("HD Audio instance list corrupted!");
    }

    //TODO: signal the handler thread for this device
    if(instance->cfg_regs->intsts.cis){

        if(instance->cfg_regs->rirb.sts.rintfl){
            int idx = instance->cfg_regs->rirb.wp % instance->rirb.entcnt;

            if(instance->cmds[idx].handler != NULL){
                instance->cmds[idx].waiting = false;
                instance->cmds[idx].handler(instance, &instance->cmds[idx], instance->rirb.buffer[idx * 2] | ((uint64_t)instance->rirb.buffer[(idx * 2) + 1] << 32));
                instance->cmds[idx].handled = true;
                instance->cmds[idx].handler = NULL;
            }
            instance->cfg_regs->rirb.sts.rintfl = 1;
        }

        if(instance->cfg_regs->sdiwake) {
            instance->codecs |= instance->cfg_regs->sdiwake;
            instance->cfg_regs->sdiwake = 0xFFFF;
        }
    }

    //DEBUG_PRINT("HD Audio Interrupt\r\n");
}

int hdaudio_sendverb(hdaudio_instance_t *instance, uint32_t addr, uint32_t node, uint32_t payload, void (*handler)(hdaudio_instance_t*, hdaudio_cmd_entry_t*, uint64_t)) {

    uint32_t verb_val = (addr << 28 | (node & 0xFF) << 20 | (payload & 0xFFFFF));

    //Allocate the cmd entry for this verb
    int idx = (instance->cfg_regs->corb.wp + 1) % instance->corb.entcnt;
    while(instance->cmds[idx].waiting && !instance->cmds[idx].handled)
        ;

    instance->cmds[idx].waiting = true;
    instance->cmds[idx].handled = false;
    instance->cmds[idx].addr = (uint8_t)addr;
    instance->cmds[idx].node = (uint8_t)node;
    instance->cmds[idx].payload = payload;
    instance->cmds[idx].handler = handler;

    //Write the verb and queue it
    instance->corb.buffer[idx] = verb_val;
    instance->cfg_regs->corb.wp++;

    return 0;
}

static void hdaudio_scanhandler(hdaudio_instance_t *instance, hdaudio_cmd_entry_t *cmd, uint64_t resp) {
    hdaudio_param_type_t param_type = GET_PARAMTYPE_FRM_PAYLOAD(cmd->payload);
    uint8_t node = cmd->node;
    uint8_t addr = cmd->addr;

    switch(param_type) {
        case hdaudio_param_vendor_device_id:
            instance->nodes[addr][node].vendor_dev_id = (uint32_t)(resp);
            break;
        case hdaudio_param_node_cnt:
            instance->nodes[addr][node].starting_sub_node = (resp >> (16)) & 0xFF;
            instance->nodes[addr][node].sub_node_cnt = (resp) & 0xFF;
            break;
        case hdaudio_param_func_grp_type:
            instance->nodes[addr][node].grp_type = (resp); 
            break;
        case hdaudio_param_audio_grp_caps:
            instance->nodes[addr][node].input_delay = (resp >> 8) & 0x0F;
            instance->nodes[addr][node].output_delay = resp & 0x0F;
            break;
        case hdaudio_param_audio_widget_caps:
            instance->nodes[addr][node].caps = (resp); 
            break;
        case hdaudio_param_pcm_rate_caps:
            instance->nodes[addr][node].pcm_rates = (resp); 
            break;
        case hdaudio_param_fmt_caps:
            instance->nodes[addr][node].stream_formats = (resp); 
            break;
        case hdaudio_param_pin_caps:
            instance->nodes[addr][node].pin_cap = (resp); 
            break;
        case hdaudio_param_input_amp_caps:
            instance->nodes[addr][node].input_amp_cap = (resp); 
            break;
        case hdaudio_param_output_amp_caps:
            instance->nodes[addr][node].output_amp_cap = (resp);
            break;
        case hdaudio_param_conn_list_len:
            instance->nodes[addr][node].conn_list_len = (resp);
            instance->nodes[addr][node].conn_list = malloc((resp & 0x7F) * sizeof(uint8_t));
            break;
        case hdaudio_param_pwr_caps:
            instance->nodes[addr][node].pwr_states = (resp);
            break;
        case hdaudio_param_processing_caps:
            instance->nodes[addr][node].process_caps = (resp);
            break;
        case hdaudio_param_gpio_cnt:
            instance->nodes[addr][node].gpio_count = (resp);
            break;
        case hdaudio_param_volume_caps:
            instance->nodes[addr][node].volume_caps = (resp);
            break;
        default:
            break;
    }
}

static void hdaudio_connlisthandler(hdaudio_instance_t *instance, hdaudio_cmd_entry_t *cmd, uint64_t resp) {
    uint8_t node = cmd->node;
    uint8_t addr = cmd->addr;
    uint8_t offset = GET_CONN_LIST_OFF(cmd->payload);

    instance->nodes[addr][node].conn_list[offset] = resp & 0xFF;
    instance->nodes[addr][node].conn_list[offset + 1] = (resp >> 8) & 0xFF;
    instance->nodes[addr][node].conn_list[offset + 2] = (resp >> 16) & 0xFF;
    instance->nodes[addr][node].conn_list[offset + 3] = (resp >> 24) & 0xFF;
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
        
    //Wait for the codecs to request state changes
    while(instance->cfg_regs->sdiwake == 0)
        ;
    DEBUG_PRINT("Codecs Enumerated!\r\n");
    
    instance->cfg_regs->gctl.crst |= (1 << 8);

    //Enable state change interrupts
    instance->cfg_regs->sdiwen = 0xFFFF;
    instance->cfg_regs->intctl.cie = 1;
    instance->cfg_regs->intctl.gie = 1;

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
    instance->cfg_regs->rirb.ctl.rintctl = 1;

    for(int i = 0; i < 32; i++){
        if(instance->codecs & (1 << i)){
            instance->nodes[i] = malloc(sizeof(hdaudio_node_t) * 256);

            for(int j = 0; j < 256; j++){
                hdaudio_sendverb(instance, i, j, GET_PARAM(hdaudio_param_vendor_device_id), hdaudio_scanhandler);
                hdaudio_sendverb(instance, i, j, GET_PARAM(hdaudio_param_node_cnt), hdaudio_scanhandler);
                hdaudio_sendverb(instance, i, j, GET_PARAM(hdaudio_param_func_grp_type), hdaudio_scanhandler);
                hdaudio_sendverb(instance, i, j, GET_PARAM(hdaudio_param_audio_grp_caps), hdaudio_scanhandler);
                hdaudio_sendverb(instance, i, j, GET_PARAM(hdaudio_param_audio_widget_caps), hdaudio_scanhandler);
                hdaudio_sendverb(instance, i, j, GET_PARAM(hdaudio_param_pcm_rate_caps), hdaudio_scanhandler);
                hdaudio_sendverb(instance, i, j, GET_PARAM(hdaudio_param_fmt_caps), hdaudio_scanhandler);
                hdaudio_sendverb(instance, i, j, GET_PARAM(hdaudio_param_pin_caps), hdaudio_scanhandler);
                hdaudio_sendverb(instance, i, j, GET_PARAM(hdaudio_param_input_amp_caps), hdaudio_scanhandler);
                hdaudio_sendverb(instance, i, j, GET_PARAM(hdaudio_param_output_amp_caps), hdaudio_scanhandler);
                hdaudio_sendverb(instance, i, j, GET_PARAM(hdaudio_param_conn_list_len), hdaudio_scanhandler);
                hdaudio_sendverb(instance, i, j, GET_PARAM(hdaudio_param_pwr_caps), hdaudio_scanhandler);
                hdaudio_sendverb(instance, i, j, GET_PARAM(hdaudio_param_processing_caps), hdaudio_scanhandler);
                hdaudio_sendverb(instance, i, j, GET_PARAM(hdaudio_param_gpio_cnt), hdaudio_scanhandler);
                hdaudio_sendverb(instance, i, j, GET_PARAM(hdaudio_param_volume_caps), hdaudio_scanhandler);
            }

            //Now read in connection lists
            for(int j = 0; j < 256; j++) {
                int list_len = instance->nodes[i][j].conn_list_len & 0x7F;
                if(instance->nodes[i][j].conn_list_len & 0x80)
                    PANIC("Long connection lists currently unsupported!");

                for(int k = 0; k < list_len; k += 4)
                    hdaudio_sendverb(instance, i, j, GET_CONN_LIST(k), hdaudio_connlisthandler);
            }
        }
    }

    DEBUG_PRINT("HD Audio initialized, Enumerating nodes\r\n");
    //Build the device graph



    return 0;
}

int module_init(void *ecam_addr) {

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
    instance->interrupt_vec = int_val;
    instance->next = NULL;

    instance->nodes = malloc(sizeof(hdaudio_node_t*) * 32);
    memset(instance->nodes, 0, sizeof(hdaudio_node_t*) * 32);

    //Synchronize device registration
    int state = cli();
    local_spinlock_lock(&device_init_lock);
    if(instances == NULL)
        instances = instance;
    else {
        instance->next = instances;
        instances = instance;
    }
    local_spinlock_unlock(&device_init_lock);
    sti(state);

    //Figure out corb and rirb size and allocate memory
    hdaudio_setupbuffersz(instance, false);
    hdaudio_setupbuffersz(instance, true);

    instance->cmds = malloc(instance->corb.entcnt * sizeof(hdaudio_cmd_entry_t));
    memset(instance->cmds, 0, sizeof(hdaudio_cmd_entry_t) * instance->corb.entcnt);

    uintptr_t corb_rirb_buffer_phys = pagealloc_alloc(0, 0, physmem_alloc_flags_data, (instance->corb.entcnt + 2 * instance->rirb.entcnt) * sizeof(uint32_t));

    uint32_t *corb_rirb_buffer = (uint32_t*)vmem_phystovirt(corb_rirb_buffer_phys, (instance->corb.entcnt + 2 * instance->rirb.entcnt) * sizeof(uint32_t), vmem_flags_uncached | vmem_flags_kernel | vmem_flags_rw);
    instance->corb.buffer = corb_rirb_buffer;
    instance->rirb.buffer = corb_rirb_buffer + instance->corb.entcnt;

    instance->corb.buffer_phys = corb_rirb_buffer_phys;
    instance->rirb.buffer_phys = corb_rirb_buffer_phys + instance->corb.entcnt * sizeof(uint32_t);

    hdaudio_initialize(instance);

    while(true)
        ;

    return 0;
}