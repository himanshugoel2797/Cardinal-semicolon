/**
 * Copyright (c) 2018 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

#include <stdlib.h>
#include <string.h>

#include "virtio.h"
#include "virtio_gpu.h"

#include "SysVirtualMemory/vmem.h"
#include "SysPhysicalMemory/phys_mem.h"

static virtio_gpu_driver_state_t device;

void intrpt_handler(int idx) {
    idx = 0;

    for(int i = 0; i < VIRTIO_GPU_VIRTQ_COUNT; i++)
        virtio_accept_used(device.common_state, i);

    virtio_gpu_config_t *cfg = (virtio_gpu_config_t*)device.common_state->dev_cfg;

    if(cfg->events_read & VIRTIO_GPU_EVENT_DISPLAY){
        DEBUG_PRINT("Display resized\r\n");
        cfg->events_clear = VIRTIO_GPU_EVENT_DISPLAY;
    }

    DEBUG_PRINT("GPU Interrupt\r\n");
}

void virtio_gpu_submitcmd(int q, void *cmd, int cmd_len, void *resp, int resp_len, void (*resp_handler)(virtio_virtq_cmd_state_t*))
{
    int cmd_idx = virtio_postcmd(device.common_state, q, cmd, cmd_len, resp, resp_len, resp_handler);
}

void virtio_gpu_default_handler(virtio_virtq_cmd_state_t *cmd) {
    //free descriptors and print any error codes
    virtio_gpu_ctrl_hdr_t *resp = (virtio_gpu_ctrl_hdr_t *)cmd->resp.virt;
    if(resp->type >= 0x1200 && resp->type <= 0x1205){
        DEBUG_PRINT("VIRTIO_GPU: ERROR\r\n");
    }

    free(cmd->cmd.virt);
    free(cmd->resp.virt);
}


void virtio_gpu_getdisplayinfo(void (*handler)(virtio_virtq_cmd_state_t*))
{
    virtio_gpu_ctrl_hdr_t *display_info_cmd = malloc(sizeof(virtio_gpu_ctrl_hdr_t));
    virtio_gpu_resp_display_info_t *display_info_resp = malloc(sizeof(virtio_gpu_resp_display_info_t));
    
    //Retrieve display info
    display_info_cmd->type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO;
    display_info_cmd->flags = 0;
    display_info_cmd->fence_id = 0;
    display_info_cmd->ctx_id = 0;
    display_info_cmd->padding = 0;

    memset(display_info_resp, 0, sizeof(virtio_gpu_resp_display_info_t));

    virtio_gpu_submitcmd(0, display_info_cmd, sizeof(virtio_gpu_ctrl_hdr_t), display_info_resp, sizeof(virtio_gpu_resp_display_info_t), handler);
}

void virtio_gpu_create2d(uint32_t id, virtio_gpu_formats_t fmt, uint32_t width, uint32_t height)
{
    virtio_gpu_resource_create_2d_t *rsc_d = malloc(sizeof(virtio_gpu_resource_create_2d_t));
    virtio_gpu_ctrl_hdr_t *resp = malloc(sizeof(virtio_gpu_ctrl_hdr_t));

    rsc_d->hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
    rsc_d->hdr.flags = 0;
    rsc_d->hdr.fence_id = 0;
    rsc_d->hdr.ctx_id = 0;
    rsc_d->hdr.padding = 0;
    
    rsc_d->resource_id = id;
    rsc_d->format = fmt;
    rsc_d->width = width;
    rsc_d->height = height;

    memset(resp, 0, sizeof(virtio_gpu_ctrl_hdr_t));

    virtio_gpu_submitcmd(0, rsc_d, sizeof(virtio_gpu_resource_create_2d_t), resp, sizeof(virtio_gpu_ctrl_hdr_t), virtio_gpu_default_handler);
}

void virtio_gpu_attachbacking(uint32_t rsc_id, uintptr_t addr, size_t len) {
    virtio_gpu_resource_attach_backing_t *rsc_d = malloc(sizeof(virtio_gpu_resource_attach_backing_t) + sizeof(virtio_gpu_mem_entry_t));
    virtio_gpu_ctrl_hdr_t *resp = malloc(sizeof(virtio_gpu_ctrl_hdr_t));

    rsc_d->hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    rsc_d->hdr.flags = 0;
    rsc_d->hdr.fence_id = 0;
    rsc_d->hdr.ctx_id = 0;
    rsc_d->hdr.padding = 0;
    
    rsc_d->resource_id = rsc_id;
    rsc_d->nr_entries = 1;
    rsc_d->entries[0].addr = addr;
    rsc_d->entries[0].length = len;
    rsc_d->entries[0].padding = 0;

    memset(resp, 0, sizeof(virtio_gpu_ctrl_hdr_t));

    virtio_gpu_submitcmd(0, rsc_d, sizeof(virtio_gpu_resource_attach_backing_t) + sizeof(virtio_gpu_mem_entry_t), resp, sizeof(virtio_gpu_ctrl_hdr_t), virtio_gpu_default_handler);
}

void virtio_gpu_setscanout(uint32_t scanout_id, uint32_t resource_id, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    virtio_gpu_set_scanout_t *rsc_d = malloc(sizeof(virtio_gpu_set_scanout_t));
    virtio_gpu_ctrl_hdr_t *resp = malloc(sizeof(virtio_gpu_ctrl_hdr_t));

    rsc_d->hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
    rsc_d->hdr.flags = 0;
    rsc_d->hdr.fence_id = 0;
    rsc_d->hdr.ctx_id = 0;
    rsc_d->hdr.padding = 0;
    
    rsc_d->resource_id = resource_id;
    rsc_d->scanout_id = scanout_id;
    rsc_d->r.x = x;
    rsc_d->r.y = y;
    rsc_d->r.width = w;
    rsc_d->r.height = h;

    memset(resp, 0, sizeof(virtio_gpu_ctrl_hdr_t));

    virtio_gpu_submitcmd(0, rsc_d, sizeof(virtio_gpu_set_scanout_t), resp, sizeof(virtio_gpu_ctrl_hdr_t), virtio_gpu_default_handler);
}

void virtio_gpu_transfertohost2d(uint32_t resource_id, uint64_t offset, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    virtio_gpu_transfer_to_host_2d_t *rsc_d = malloc(sizeof(virtio_gpu_transfer_to_host_2d_t));
    virtio_gpu_ctrl_hdr_t *resp = malloc(sizeof(virtio_gpu_ctrl_hdr_t));

    rsc_d->hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    rsc_d->hdr.flags = 0;
    rsc_d->hdr.fence_id = 0;
    rsc_d->hdr.ctx_id = 0;
    rsc_d->hdr.padding = 0;
    
    rsc_d->offset = offset;
    rsc_d->resource_id = resource_id;
    rsc_d->padding = 0;
    rsc_d->r.x = x;
    rsc_d->r.y = y;
    rsc_d->r.width = w;
    rsc_d->r.height = h;

    memset(resp, 0, sizeof(virtio_gpu_ctrl_hdr_t));

    virtio_gpu_submitcmd(0, rsc_d, sizeof(virtio_gpu_transfer_to_host_2d_t), resp, sizeof(virtio_gpu_ctrl_hdr_t), virtio_gpu_default_handler);
}

void virtio_gpu_flush(uint32_t resource_id, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    virtio_gpu_resource_flush_t *rsc_d = malloc(sizeof(virtio_gpu_resource_flush_t));
    virtio_gpu_ctrl_hdr_t *resp = malloc(sizeof(virtio_gpu_ctrl_hdr_t));

    rsc_d->hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    rsc_d->hdr.flags = 0;
    rsc_d->hdr.fence_id = 0;
    rsc_d->hdr.ctx_id = 0;
    rsc_d->hdr.padding = 0;
    
    rsc_d->resource_id = resource_id;
    rsc_d->padding = 0;
    rsc_d->r.x = x;
    rsc_d->r.y = y;
    rsc_d->r.width = w;
    rsc_d->r.height = h;

    memset(resp, 0, sizeof(virtio_gpu_ctrl_hdr_t));

    virtio_gpu_submitcmd(0, rsc_d, sizeof(virtio_gpu_resource_flush_t), resp, sizeof(virtio_gpu_ctrl_hdr_t), virtio_gpu_default_handler);
}

void virtio_gpu_displayinit_handler(virtio_virtq_cmd_state_t *cmd) {

    //read the display info, initialize all enabled scanouts
    virtio_gpu_resp_display_info_t *display_info = (virtio_gpu_resp_display_info_t*)cmd->resp.virt;

    for(int i = 0; i < VIRTIO_GPU_MAX_SCANOUTS; i++){
        if(display_info->pmodes[i].enabled){
            //setup this scanout

            //create a 2d resource
            virtio_gpu_create2d(i + 1, VIRTIO_GPU_FORMAT_X8R8G8B8_UNORM, display_info->pmodes[i].r.width, display_info->pmodes[i].r.height);

            //set its backing data
            size_t sz = display_info->pmodes[i].r.width * display_info->pmodes[i].r.height * sizeof(uint32_t);
            uintptr_t fbuf = pagealloc_alloc(0, 0, physmem_alloc_flags_data, sz);
            virtio_gpu_attachbacking(i + 1, fbuf, sz);

            //set scanout
            virtio_gpu_setscanout(i, i + 1, 0, 0, display_info->pmodes[i].r.width, display_info->pmodes[i].r.height);

            uint32_t *vaddr = (uint32_t*)vmem_phystovirt(fbuf, sz, vmem_flags_uncached | vmem_flags_kernel | vmem_flags_rw);
            memset(vaddr, 0xff, sz);

            virtio_gpu_transfertohost2d(i + 1, 0, 0, 0, display_info->pmodes[i].r.width, display_info->pmodes[i].r.height);
            virtio_gpu_flush(i + 1, 0, 0, display_info->pmodes[i].r.width, display_info->pmodes[i].r.height);

            virtio_notify(device.common_state, 0);
        }
    }

    virtio_gpu_default_handler(cmd);
}

int module_init(void *ecam) {

    memset(&device, 0, sizeof(device));

    device.qstate[0] = device.controlq;
    device.qstate[1] = device.cursorq;
    device.common_state = virtio_initialize(ecam, intrpt_handler, device.qstate);

    uint32_t features = virtio_getfeatures(device.common_state, 0);

    device.virgl_mode = false;
    if(features & VIRTIO_GPU_F_VIRGL){
        DEBUG_PRINT("VirGL Support Enabled.\r\n");
        device.virgl_mode = true;
        virtio_setfeatures(device.common_state, VIRTIO_GPU_F_VIRGL | VIRTIO_F_INDIRECT_DESC , 0);
    }else
        virtio_setfeatures(device.common_state, VIRTIO_F_INDIRECT_DESC , 0);

    if(!virtio_features_ok(device.common_state))
        return -1;

    //setup virtqueues
    virtio_setupqueue(device.common_state, 0, VIRTIO_GPU_VIRTQ_LEN);
    virtio_setupqueue(device.common_state, 1, VIRTIO_GPU_VIRTQ_LEN);

    virtio_driver_ok(device.common_state);

    virtio_gpu_getdisplayinfo(virtio_gpu_displayinit_handler);
    virtio_notify(device.common_state, 0);

    while(true) {
    }

    return 0;
}