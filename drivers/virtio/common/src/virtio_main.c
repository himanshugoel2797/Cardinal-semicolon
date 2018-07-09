/**
 * Copyright (c) 2018 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

#include <types.h>
#include <stdlib.h>
#include <string.h>

#include "SysVirtualMemory/vmem.h"
#include "SysPhysicalMemory/phys_mem.h"
#include "SysInterrupts/interrupts.h"
#include "pci/pci.h"
#include "virtio.h"

void tmp_handler(int i) {
    i = 0;
}

PRIVATE virtio_state_t* virtio_initialize(void *ecam_addr, void (*int_handler)(int), virtio_virtq_cmd_state_t **cmds, int *avail_idx, int *used_idx) {
    pci_config_t *device = (pci_config_t*)vmem_phystovirt((intptr_t)ecam_addr, KiB(4), vmem_flags_uncached | vmem_flags_kernel | vmem_flags_rw);

    //enable pci bus master
    device->command.busmaster = 1;
    device->command.mem_space = 1;
    device->command.int_disable = 1;

    virtio_state_t *n_state = malloc(sizeof(virtio_state_t));
    n_state->cmds = cmds;
    n_state->device = device;
    n_state->avail_idx = avail_idx;
    n_state->used_idx = used_idx;

    int msi_vector = 0;

    //TODO: install msi interrupt servicing
    //traverse capabilities
    if(device->capabilitiesPtr != 0) {
        uint8_t ptr = device->capabilitiesPtr;
        uint8_t *pci_base = (uint8_t*)device;

        do {
            pci_cap_header_t *capEntry = (pci_cap_header_t*)(pci_base + ptr);

            if(capEntry->capID == pci_cap_msi || capEntry->capID == pci_cap_msix) {
                DEBUG_PRINT("MSIX\r\n");

                //interrupt setup
                int int_cnt = 0;
                int msi_val = pci_getmsiinfo(device, &int_cnt);

                interrupt_allocate(1, interrupt_flags_exclusive, &msi_vector);
                interrupt_registerhandler(msi_vector, int_handler);

                uintptr_t msi_addr = (uintptr_t)msi_register_addr(0);
                uint32_t msi_msg = msi_register_data(msi_vector);
                pci_setmsiinfo(device, msi_val, &msi_addr, &msi_msg, 1);

            } else if(capEntry->capID == pci_cap_vendor) {
                intptr_t bar_addr = 0;

                virtio_pci_cap_t *vendorCap = (virtio_pci_cap_t*)capEntry;
                if(device->bar[vendorCap->bar] & 4)
                    bar_addr = ((intptr_t)device->bar[vendorCap->bar + 1] << 32) + device->bar[vendorCap->bar] & 0xFFFF0000;
                else
                    bar_addr = device->bar[vendorCap->bar] & 0xFFFF0000;

                bar_addr = vmem_phystovirt(bar_addr, KiB(4), vmem_flags_uncached | vmem_flags_kernel | vmem_flags_rw);

                switch(vendorCap->cfg_type) {
                case virtio_pci_cap_cfg_common: {
                    n_state->common_cfg = (virtio_pci_common_cfg_t*)(bar_addr + vendorCap->offset);
                }
                break;
                case virtio_pci_cap_cfg_notify: {
                    n_state->notif_cfg = (virtio_pci_notif_cfg_t*)vendorCap;
                    n_state->notif_bar = bar_addr;
                }
                break;
                case virtio_pci_cap_cfg_isr: {
                    n_state->isr_cfg = (volatile uint32_t*)(bar_addr + vendorCap->offset);
                }
                break;
                case virtio_pci_cap_cfg_device: {
                    n_state->dev_cfg = (void*)(bar_addr + vendorCap->offset);
                }
                break;
                case virtio_pci_cap_cfg_pci:
                    //Don't care, we don't need the alternative access mechanisms
                    break;
                }
            }
            ptr = capEntry->nextPtr;
        } while(ptr != 0);
    }

    //Set ack bit
    //Set driver bit
    n_state->common_cfg->device_status |= (ACKNOWLEDGE);
    n_state->common_cfg->device_status |= (DRIVER);
    n_state->common_cfg->msix_config = 0;


    return n_state;
}

//Read device features, write supported features
PRIVATE uint32_t virtio_getfeatures(virtio_state_t *state, int idx) {
    state->common_cfg->device_feature_select = idx;
    return state->common_cfg->device_feature;
}

PRIVATE void virtio_setfeatures(virtio_state_t *state, int idx, uint32_t val) {
    state->common_cfg->driver_feature_select = idx;
    state->common_cfg->driver_feature = val;
}

PRIVATE bool virtio_features_ok(virtio_state_t *state) {
    //set FEATURES_OK
    //re-read FEATURES_OK bit to confirm it was accepted
    state->common_cfg->device_status |= (FEATURES_OK);
    return state->common_cfg->device_status & (FEATURES_OK);
}

PRIVATE void virtio_driver_ok(virtio_state_t *state) {
    state->common_cfg->device_status |= (DRIVER_OK);
}

PRIVATE void* virtio_setupqueue(virtio_state_t *state, int idx, int entcnt) {

    state->common_cfg->queue_select = (uint16_t)idx;
    state->common_cfg->queue_size = entcnt;

    size_t ts = 16 * entcnt + 6 + 2 * entcnt + 6 + 8 * entcnt;

    uintptr_t virtqueue_phys = pagealloc_alloc(0, 0, physmem_alloc_flags_data, ts);
    intptr_t virtqueue_virt = vmem_phystovirt((intptr_t)virtqueue_phys, ts, vmem_flags_uncached | vmem_flags_kernel | vmem_flags_rw);

    memset((void*)virtqueue_virt, 0, ts);

    state->common_cfg->queue_desc = virtqueue_phys;
    state->common_cfg->queue_avail = virtqueue_phys + 16 * entcnt;
    state->common_cfg->queue_used = virtqueue_phys + 16 * entcnt + 6 + 2 * entcnt;
    state->common_cfg->queue_msix_vector = 0;
    state->common_cfg->queue_enable = 1;

    return (void*)virtqueue_virt;
}

PRIVATE void virtio_notify(virtio_state_t *state, int idx) {
    state->common_cfg->queue_select = (uint16_t)idx;

    *(uint16_t*)(state->notif_bar + state->notif_cfg->cap.offset + state->common_cfg->queue_notify_off * state->notif_cfg->notify_multiplier)
        = (uint16_t)idx;
}

PRIVATE void virtio_addresponse(virtio_state_t *state, int idx, void *buf, int len) {
    state->common_cfg->queue_select = (uint16_t)idx;
    int q_len = state->common_cfg->queue_size;

    virtq_desc_t *descs = (virtq_desc_t*) vmem_phystovirt((intptr_t)state->common_cfg->queue_desc, q_len * 16, vmem_flags_uncached | vmem_flags_kernel | vmem_flags_rw);
    virtq_avail_t *avails = (virtq_avail_t*) vmem_phystovirt((intptr_t)state->common_cfg->queue_avail, q_len * 2 + 6, vmem_flags_uncached | vmem_flags_kernel | vmem_flags_rw);
    //avails->flags = 1;

    int cur_desc_idx = state->avail_idx[idx];
    state->avail_idx[idx] = (state->avail_idx[idx] + 1) % q_len;

    while(state->cmds[idx][cur_desc_idx].waiting && !state->cmds[idx][cur_desc_idx].finished) {
        virtio_notify(state, idx);
    }

    state->cmds[idx][cur_desc_idx].waiting = true;
    state->cmds[idx][cur_desc_idx].finished = false;
    state->cmds[idx][cur_desc_idx].q = idx;
    state->cmds[idx][cur_desc_idx].idx = cur_desc_idx;
    state->cmds[idx][cur_desc_idx].handler = NULL;

    //Fill a descriptor with the cmd and update the available ring
    //Fill a descriptor with the response and update the available ring
    intptr_t resp_phys = 0;
    vmem_virttophys((intptr_t)buf, &resp_phys);

    {
        state->cmds[idx][cur_desc_idx].resp.virt = buf;
        state->cmds[idx][cur_desc_idx].resp.phys = resp_phys;
        state->cmds[idx][cur_desc_idx].resp.len = len;
    }

    descs[cur_desc_idx].addr = resp_phys;
    descs[cur_desc_idx].len = (uint32_t)len;
    descs[cur_desc_idx].flags = VIRTQ_DESC_F_WRITE;
    descs[cur_desc_idx].next = 0;

    avails->ring[avails->idx % q_len] = cur_desc_idx;
    avails->idx ++;
}

PRIVATE void virtio_postcmd_noresp(virtio_state_t *state, int idx, void *cmd, int len, void (*resp_handler)(virtio_virtq_cmd_state_t*)) {

    state->common_cfg->queue_select = (uint16_t)idx;
    int q_len = state->common_cfg->queue_size;

    virtq_desc_t *descs = (virtq_desc_t*) vmem_phystovirt((intptr_t)state->common_cfg->queue_desc, q_len * 16, vmem_flags_uncached | vmem_flags_kernel | vmem_flags_rw);
    virtq_avail_t *avails = (virtq_avail_t*) vmem_phystovirt((intptr_t)state->common_cfg->queue_avail, q_len * 2 + 6, vmem_flags_uncached | vmem_flags_kernel | vmem_flags_rw);
    virtq_used_t *used = (virtq_used_t*) vmem_phystovirt((intptr_t)state->common_cfg->queue_used, q_len * 8 + 6, vmem_flags_uncached | vmem_flags_kernel | vmem_flags_rw);
    //avails->flags = 1;

    if(used->idx == 0)
        DEBUG_PRINT("Z\r\n");

    int cur_desc_idx = state->avail_idx[idx];
    state->avail_idx[idx] = (state->avail_idx[idx] + 1) % q_len;

    while(state->cmds[idx][cur_desc_idx].waiting && !state->cmds[idx][cur_desc_idx].finished) {
        virtio_notify(state, idx);
    }

    state->cmds[idx][cur_desc_idx].waiting = true;
    state->cmds[idx][cur_desc_idx].finished = false;
    state->cmds[idx][cur_desc_idx].q = idx;
    state->cmds[idx][cur_desc_idx].idx = cur_desc_idx;
    state->cmds[idx][cur_desc_idx].handler = resp_handler;

    //Fill a descriptor with the cmd and update the available ring
    //Fill a descriptor with the response and update the available ring
    intptr_t cmd_phys = 0;
    vmem_virttophys((intptr_t)cmd, &cmd_phys);

    {
        state->cmds[idx][cur_desc_idx].cmd.virt = cmd;
        state->cmds[idx][cur_desc_idx].cmd.phys = cmd_phys;
        state->cmds[idx][cur_desc_idx].cmd.len = len;
    }

    descs[cur_desc_idx].next = 0;
    descs[cur_desc_idx].len = (uint32_t)len;
    descs[cur_desc_idx].flags = 0;
    descs[cur_desc_idx].addr = cmd_phys;
    //__asm__("cli\n\thlt" :: "a"(used->idx), "b"(len));

    avails->ring[avails->idx % q_len] = cur_desc_idx;
    avails->idx ++;

}

PRIVATE void virtio_postcmd(virtio_state_t *state, int idx, void *cmd, int len, void *resp, int response_len, void (*resp_handler)(virtio_virtq_cmd_state_t*)) {

    state->common_cfg->queue_select = (uint16_t)idx;
    int q_len = state->common_cfg->queue_size;

    virtq_desc_t *descs = (virtq_desc_t*) vmem_phystovirt((intptr_t)state->common_cfg->queue_desc, q_len * 16, vmem_flags_uncached | vmem_flags_kernel | vmem_flags_rw);
    virtq_avail_t *avails = (virtq_avail_t*) vmem_phystovirt((intptr_t)state->common_cfg->queue_avail, q_len * 2 + 6, vmem_flags_uncached | vmem_flags_kernel | vmem_flags_rw);
    //avails->flags = 1;

    int cur_desc_idx = state->avail_idx[idx];
    state->avail_idx[idx] = (state->avail_idx[idx] + 2) % q_len;

    while(state->cmds[idx][cur_desc_idx].waiting && !state->cmds[idx][cur_desc_idx].finished) {
        virtio_notify(state, idx);
    }

    state->cmds[idx][cur_desc_idx].waiting = true;
    state->cmds[idx][cur_desc_idx].finished = false;
    state->cmds[idx][cur_desc_idx].q = idx;
    state->cmds[idx][cur_desc_idx].idx = cur_desc_idx;
    state->cmds[idx][cur_desc_idx].handler = resp_handler;

    //Fill a descriptor with the cmd and update the available ring
    //Fill a descriptor with the response and update the available ring
    intptr_t cmd_phys = 0;
    intptr_t resp_phys = 0;
    vmem_virttophys((intptr_t)cmd, &cmd_phys);
    if(response_len > 0)vmem_virttophys((intptr_t)resp, &resp_phys);

    {
        state->cmds[idx][cur_desc_idx].cmd.virt = cmd;
        state->cmds[idx][cur_desc_idx].cmd.phys = cmd_phys;
        state->cmds[idx][cur_desc_idx].cmd.len = len;

        state->cmds[idx][cur_desc_idx].resp.virt = resp;
        state->cmds[idx][cur_desc_idx].resp.phys = resp_phys;
        state->cmds[idx][cur_desc_idx].resp.len = response_len;
    }

    descs[cur_desc_idx].addr = cmd_phys;
    descs[cur_desc_idx].len = (uint32_t)len;
    descs[cur_desc_idx].flags = 0;
    descs[cur_desc_idx].next = 0;

    if(response_len > 0) {
        descs[cur_desc_idx].flags = VIRTQ_DESC_F_NEXT;
        descs[cur_desc_idx].next = cur_desc_idx + 1;

        descs[cur_desc_idx + 1].addr = resp_phys;
        descs[cur_desc_idx + 1].len = response_len;
        descs[cur_desc_idx + 1].flags = VIRTQ_DESC_F_WRITE;
        descs[cur_desc_idx + 1].next = 0;
    }

    avails->ring[avails->idx % q_len] = cur_desc_idx;
    avails->idx ++;
}

void virtio_accept_used(virtio_state_t *state, int idx) {
    state->common_cfg->queue_select = (uint16_t)idx;
    int q_len = state->common_cfg->queue_size;

    virtq_used_t *descs = (virtq_used_t*) vmem_phystovirt((intptr_t)state->common_cfg->queue_used, q_len * 8 + 6, vmem_flags_uncached | vmem_flags_kernel | vmem_flags_rw);

    int d_idx = descs->idx;
    do {
        for(int i = state->used_idx[idx]; i != d_idx; i++) {
            int r_id = descs->ring[i % q_len].id;

            state->cmds[idx][r_id].waiting = false;
            if(state->cmds[idx][r_id].handler != NULL)
                state->cmds[idx][r_id].handler(&state->cmds[idx][r_id]);
            state->cmds[idx][r_id].finished = true;
            state->cmds[idx][r_id].handler = NULL;
        }
        if(d_idx == descs->idx) {
            *state->isr_cfg;
            return;
        } else {
            state->used_idx[idx] = d_idx;
            d_idx = descs->idx;
        }
    } while(true);
}