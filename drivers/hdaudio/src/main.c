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

int hdaudio_setupbuffersz(hdaudio_instance_t *instance, bool corb)
{
    uint32_t entcnt_cap = instance->cfg_regs->rirb.sz.szcap;
    if (corb)
    {
        entcnt_cap = instance->cfg_regs->corb.sz.szcap;
    }

    if (entcnt_cap & (1 << 2))
        entcnt_cap = 256;
    else if (entcnt_cap & (1 << 1))
        entcnt_cap = 16;
    else if (entcnt_cap & (1 << 0))
        entcnt_cap = 2;

    if (corb)
    {
        instance->corb.entcnt = entcnt_cap;
    }
    else
    {
        instance->rirb.entcnt = entcnt_cap;
    }

    switch (entcnt_cap)
    {
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

    if (corb)
    {
        instance->cfg_regs->corb.sz.sz = entcnt_cap;
    }
    else
    {
        instance->cfg_regs->rirb.sz.sz = entcnt_cap;
    }

    return 0;
}

static void tmp_handler(int int_num)
{
    hdaudio_instance_t *instance = instances;
    while (instance->interrupt_vec != int_num)
    {
        instance = instance->next;

        if (instance == NULL)
            PANIC("[HDAudio] instance list corrupted!");
    }

    //TODO: signal the handler thread for this device
    if (instance->cfg_regs->intsts & 0xc0000000)
    {
        if (instance->cfg_regs->rirb.sts.rintfl)
        {
            instance->rirb_rp++;

            int idx = instance->cfg_regs->rirb.wp % instance->rirb.entcnt;
            int cmd_idx = instance->rirb_rp % instance->corb.entcnt;

            if (instance->rirb.buffer[(idx * 2) + 1] & (1 << 4))
            {
                //Unsolicited response
                instance->rirb_rp--;
            }
            else if (instance->cmds[cmd_idx].handler != NULL)
            {
                //Solicited response
                instance->cmds[cmd_idx].waiting = false;
                instance->cmds[cmd_idx].handler(instance, &instance->cmds[cmd_idx], instance->rirb.buffer[idx * 2]);
                instance->cmds[cmd_idx].handled = true;
                instance->cmds[cmd_idx].handler = NULL;
            }
            instance->cfg_regs->rirb.sts.rintfl = 1;
        }

        if (instance->cfg_regs->statests)
        {
            instance->codecs |= instance->cfg_regs->statests;
            instance->cfg_regs->statests = 0xFFFF;
        }
    }

    //DEBUG_PRINT("HD Audio Interrupt\r\n");
}

int hdaudio_sendverb(hdaudio_instance_t *instance, uint32_t addr, uint32_t node, uint32_t payload, void (*handler)(hdaudio_instance_t *, hdaudio_cmd_entry_t *, uint64_t))
{

    uint32_t verb_val = (addr << 28 | (node & 0x7F) << 20 | (payload & 0xFFFFF));

    //Allocate the cmd entry for this verb
    int idx = (instance->cfg_regs->corb.wp + 1) % instance->corb.entcnt;
    while (instance->cmds[idx].waiting && !instance->cmds[idx].handled)
        halt() //DEBUG_PRINT("WAITING\r\n");
            ;

    instance->cmds[idx].waiting = true;
    instance->cmds[idx].handled = false;
    instance->cmds[idx].addr = (uint8_t)addr;
    instance->cmds[idx].node = (uint8_t)node;
    instance->cmds[idx].payload = payload;
    instance->cmds[idx].handler = handler;

    //Write the verb and queue it
    instance->corb.buffer[idx] = verb_val;
    instance->cfg_regs->corb.wp = idx & 0xFF;
    instance->cfg_regs->rirb.sts.rintfl = 1;

    //char tmp[10];
    //DEBUG_PRINT("USED: ");
    //DEBUG_PRINT(itoa(instance->cfg_regs->corb.rp, tmp, 16));
    //DEBUG_PRINT("\r\n");

    //while (instance->cfg_regs->corb.rp < instance->cfg_regs->corb.wp)
    //    halt();
    //DEBUG_PRINT("HOLDING\r\n");

    return idx;
}

void hdaudio_waitcmd(hdaudio_instance_t *instance, int idx)
{
    while (!instance->cmds[idx].handled)
        halt();
}

static volatile _Atomic int max_node_id = 1;
static void hdaudio_scanhandler(hdaudio_instance_t *instance, hdaudio_cmd_entry_t *cmd, uint64_t resp)
{
    hdaudio_param_type_t param_type = GET_PARAMTYPE_FRM_PAYLOAD(cmd->payload);
    uint8_t node = cmd->node;
    uint8_t addr = cmd->addr;

    if (cmd->payload == GET_CFG_DEFAULT)
    {
        instance->nodes[addr][node].configuration_default = resp;
        return;
    }

    if (cmd->payload == GET_CONN_SEL)
    {
        instance->nodes[addr][node].current_index = resp & 0x7F;

        char tmp[10];
        DEBUG_PRINT("[HDAudio] Connection Idx: ");
        DEBUG_PRINT(itoa(resp & 0x7F, tmp, 16));
        DEBUG_PRINT("\r\n");
        return;
    }

    switch (param_type)
    {
    case hdaudio_param_vendor_device_id:
        instance->nodes[addr][node].vendor_dev_id = (uint32_t)(resp);
        break;
    case hdaudio_param_node_cnt:
    {
        instance->nodes[addr][node].starting_sub_node = (resp >> (16)) & 0xFF;
        instance->nodes[addr][node].sub_node_cnt = (resp)&0xFF;

        if (instance->nodes[addr][node].sub_node_cnt != 0)
        {
            int max_node_id_l = instance->nodes[addr][node].starting_sub_node + instance->nodes[addr][node].sub_node_cnt;
            if (max_node_id_l > max_node_id)
                max_node_id = max_node_id_l;
        }

        char tmp[10];
        DEBUG_PRINT("[HDAudio] Parent Node: ");
        DEBUG_PRINT(itoa(node, tmp, 16));

        DEBUG_PRINT("\tChild Node Base: ");
        DEBUG_PRINT(itoa((resp >> (16)) & 0xFF, tmp, 16));

        DEBUG_PRINT("\tChild Node Count: ");
        DEBUG_PRINT(itoa(resp & 0xFF, tmp, 16));

        DEBUG_PRINT("\r\n");
    }
    break;
    case hdaudio_param_func_grp_type:
        instance->nodes[addr][node].grp_type = (resp);
        break;
    case hdaudio_param_audio_grp_caps:
        instance->nodes[addr][node].input_delay = (resp >> 8) & 0x0F;
        instance->nodes[addr][node].output_delay = resp & 0x0F;
        break;
    case hdaudio_param_audio_widget_caps:
    {
        instance->nodes[addr][node].caps = (resp);
        instance->nodes[addr][node].widgetType = (resp >> 20) & 0xF;

        char tmp[10];
        DEBUG_PRINT("[HDAudio] Widget Type: ");
        DEBUG_PRINT(itoa((resp >> 20) & 0xF, tmp, 16));
        DEBUG_PRINT("\r\n");
    }
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
    {
        instance->nodes[addr][node].conn_list_len = (resp);
        instance->nodes[addr][node].conn_list = malloc((resp & 0x7F) * sizeof(uint8_t));

        char tmp[10];
        DEBUG_PRINT("[HDAudio] Connection Count: ");
        DEBUG_PRINT(itoa(resp & 0x7F, tmp, 16));
        DEBUG_PRINT("\r\n");
    }
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

static void hdaudio_connlisthandler(hdaudio_instance_t *instance, hdaudio_cmd_entry_t *cmd, uint64_t resp)
{
    uint8_t node = cmd->node;
    uint8_t addr = cmd->addr;
    uint8_t offset = GET_CONN_LIST_OFF(cmd->payload);
    uint8_t conn_list_len = instance->nodes[addr][node].conn_list_len & 0x7F;

    if (conn_list_len > offset)
        instance->nodes[addr][node].conn_list[offset] = resp & 0xFF;
    if (conn_list_len > offset + 1)
        instance->nodes[addr][node].conn_list[offset + 1] = (resp >> 8) & 0xFF;
    if (conn_list_len > offset + 2)
        instance->nodes[addr][node].conn_list[offset + 2] = (resp >> 16) & 0xFF;
    if (conn_list_len > offset + 3)
        instance->nodes[addr][node].conn_list[offset + 3] = (resp >> 24) & 0xFF;
}

int hdaudio_initialize(hdaudio_instance_t *instance)
{
    //Wait for the codecs to request state changes
    //while (instance->cfg_regs->statests == 0)
    //    ;
    //instance->cfg_regs->gctl.crst |= (1 << 8);

    //Enable state change interrupts
    instance->cfg_regs->wakeen = 0xFFFF;

    DEBUG_PRINT("[HDAudio] Codecs Enumerated!\r\n");
    instance->codecs = instance->cfg_regs->statests;

    //Configure CORB
    instance->cfg_regs->corb.ctl.corbrun = 0;
    while (instance->cfg_regs->corb.ctl.corbrun)
        ;

    instance->cfg_regs->corb.lower_base = (uint32_t)instance->corb.buffer_phys;
    instance->cfg_regs->corb.upper_base = (uint32_t)(instance->corb.buffer_phys >> 32);

    instance->cfg_regs->corb.wp = 0;

    //Reset the CORB read pointer
    instance->cfg_regs->corb.rp = (1 << 15);
    while (!(instance->cfg_regs->corb.rp & (1 << 15)))
        ;
    instance->cfg_regs->corb.rp = 0;
    while (instance->cfg_regs->corb.rp & (1 << 15))
        ;

    //Configure RIRB
    instance->cfg_regs->rirb.ctl.dmaen = 0;
    while (instance->cfg_regs->rirb.ctl.dmaen)
        ;

    instance->cfg_regs->rirb.lower_base = (uint32_t)instance->rirb.buffer_phys;
    instance->cfg_regs->rirb.upper_base = (uint32_t)(instance->rirb.buffer_phys >> 32);

    //Reset the RIRB write pointer
    instance->cfg_regs->rirb.wp |= (1 << 15);

    //Set the number of messages after which to interrupt
    instance->cfg_regs->rirb.intcnt = 1;
    instance->cfg_regs->intctl = 0xc0000000;
    instance->cfg_regs->rirb.ctl.rintctl = 1;

    //Start the rirb again
    instance->cfg_regs->rirb.ctl.dmaen = 1;
    while (!instance->cfg_regs->rirb.ctl.dmaen)
        ;

    //Start the corb again
    instance->cfg_regs->corb.ctl.corbrun = 1;
    while (!instance->cfg_regs->corb.ctl.corbrun)
        ;

    //Build the device graph
    DEBUG_PRINT("[HDAudio] Controller Initialized, Enumerating nodes\r\n");
    max_node_id = 1;
    for (int i = 0; i < 16; i++)
    {
        if (instance->codecs & (1u << i))
        {
            instance->nodes[i] = malloc(sizeof(hdaudio_node_t) * 256);

            for (int j = 0; j < max_node_id; j++)
            {
                instance->nodes[i][j].isValid = true;

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
                hdaudio_sendverb(instance, i, j, GET_CONN_SEL, hdaudio_scanhandler);
                int fcmd = hdaudio_sendverb(instance, i, j, GET_CFG_DEFAULT, hdaudio_scanhandler);
                hdaudio_waitcmd(instance, fcmd);
            }

            //Now read in connection lists
            for (int j = 0; j < max_node_id; j++)
            {
                int list_len = instance->nodes[i][j].conn_list_len & 0x7F;
                if (instance->nodes[i][j].conn_list_len & 0x80)
                    PANIC("[HDAudio] Long connection lists currently unsupported!");

                int fcmd = -1;
                for (int k = 0; k < list_len; k += 4)
                    fcmd = hdaudio_sendverb(instance, i, j, GET_CONN_LIST(k), hdaudio_connlisthandler);
                if (fcmd >= 0)
                    hdaudio_waitcmd(instance, fcmd);
            }

            //Sort everything into groups based on widget type
            uint8_t *node_grps[7];
            uint8_t node_grps_cnt[7];
            memset(node_grps, 0, sizeof(uint8_t *) * 7);

            for (int j = 1; j < max_node_id; j++)
            {
                if (instance->nodes[i][j].caps == 0)
                    continue;

                int node_type = (instance->nodes[i][j].caps >> 20) & 0xF;

                if (node_type < 0x7)
                {
                    if (node_grps[node_type] == NULL)
                    {
                        node_grps[node_type] = malloc(sizeof(uint8_t) * max_node_id);
                        memset(node_grps[node_type], 0, max_node_id * sizeof(uint8_t));

                        node_grps_cnt[node_type] = 0;
                    }

                    node_grps[node_type][node_grps_cnt[node_type]++] = j;
                }
            }

            //Use the default connections for now
            //Sort pins by input and output
            for (uint8_t j = 0; j < node_grps_cnt[hdaudio_widget_pin_complex]; j++)
            {
            }

            //For each pin, build a shortest path from pin to stream
            //Output pins must end at Audio Outputs and must include an amplifier in-between
            //Input pins must end at Audio Inputs and must include an amplifier in-between
            /*
            for(int j = 0; j < node_grps_cnt[hdaudio_widget_pin_complex]; j++) {
                hdaudio_node_t *cur_node = &instance->nodes[i][node_grps[hdaudio_widget_pin_complex][j]];

                bool isOutput = cur_node->pin_cap & (1 << 4);
                bool isInput = cur_node->pin_cap & (1 << 5);

                uint32_t input_delay = cur_node->input_delay;
                uint32_t output_delay = cur_node->output_delay;

                //walk the connection list
                int conn_list_len = cur_node->conn_list_len & 0x7F;

                for(int k = 0; k < conn_list_len; k++) {

                }
            }*/

            //Paths have a maximum length of 10 units
        }
    }

    return 0;
}

int module_init(void *ecam_addr)
{

    pci_config_t *device = (pci_config_t *)vmem_phystovirt((intptr_t)ecam_addr, KiB(4), vmem_flags_uncached | vmem_flags_kernel | vmem_flags_rw);

    //enable pci bus master
    device->command.busmaster = 1;

    //interrupt setup
    int int_cnt = 0;
    int msi_val = pci_getmsiinfo(device, &int_cnt);

    if (msi_val < 0)
        DEBUG_PRINT("[HDAudio] NO MSI\r\n");

    int int_val = 0;
    interrupt_allocate(1, interrupt_flags_none, &int_val);
    interrupt_registerhandler(int_val, tmp_handler);
    {
        DEBUG_PRINT("[HDAudio] Allocated Interrupt Vector: ");
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
        if ((device->bar[i] & 0x6) == 0x4) //Is 64-bit
            bar = (device->bar[i] & 0xFFFFFFF0) + ((uint64_t)device->bar[i + 1] << 32);
        else if ((device->bar[i] & 0x6) == 0x0) //Is 32-bit
            bar = (device->bar[i] & 0xFFFFFFF0);
        if (bar)
            break;
    }

    //create the device entry
    hdaudio_instance_t *instance = (hdaudio_instance_t *)malloc(sizeof(hdaudio_instance_t));
    instance->cfg_regs = (hdaudio_regs_t *)vmem_phystovirt(bar, KiB(4), vmem_flags_uncached | vmem_flags_kernel | vmem_flags_rw);
    instance->interrupt_vec = int_val;
    instance->next = NULL;

    instance->nodes = malloc(sizeof(hdaudio_node_t *) * 32);
    memset(instance->nodes, 0, sizeof(hdaudio_node_t *) * 32);

    //Synchronize device registration
    int state = cli();
    local_spinlock_lock(&device_init_lock);
    if (instances == NULL)
        instances = instance;
    else
    {
        instance->next = instances;
        instances = instance;
    }
    local_spinlock_unlock(&device_init_lock);
    sti(state);

    //bring the device out of reset
    //instance->cfg_regs->statests = 0xFFFF;

    instance->cfg_regs->gctl.crst = 0;
    while (instance->cfg_regs->gctl.crst != 0)
        ;
    instance->cfg_regs->gctl.crst = 1;
    while (instance->cfg_regs->gctl.crst != 1)
        ;

    //Figure out corb and rirb size and allocate memory
    hdaudio_setupbuffersz(instance, false);
    hdaudio_setupbuffersz(instance, true);

    instance->cmds = malloc(instance->corb.entcnt * sizeof(hdaudio_cmd_entry_t));
    memset(instance->cmds, 0, sizeof(hdaudio_cmd_entry_t) * instance->corb.entcnt);

    uintptr_t corb_rirb_buffer_phys = pagealloc_alloc(0, 0, physmem_alloc_flags_data | physmem_alloc_flags_zero, (instance->corb.entcnt + 2 * instance->rirb.entcnt) * sizeof(uint32_t));

    uint32_t *corb_rirb_buffer = (uint32_t *)vmem_phystovirt(corb_rirb_buffer_phys, (instance->corb.entcnt + 2 * instance->rirb.entcnt) * sizeof(uint32_t), vmem_flags_uncached | vmem_flags_kernel | vmem_flags_rw);
    instance->corb.buffer = corb_rirb_buffer;
    instance->rirb.buffer = corb_rirb_buffer + instance->corb.entcnt;

    instance->corb.buffer_phys = corb_rirb_buffer_phys;
    instance->rirb.buffer_phys = corb_rirb_buffer_phys + instance->corb.entcnt * sizeof(uint32_t);

    hdaudio_initialize(instance);

    return 0;
}