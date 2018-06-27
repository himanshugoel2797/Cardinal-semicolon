/**
 * Copyright (c) 2018 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

#include <stdlib.h>
#include <string.h>
#include <cardinal/local_spinlock.h>

#include "virtio.h"
#include "virtio_gpu.h"

#include "SysVirtualMemory/vmem.h"
#include "SysPhysicalMemory/phys_mem.h"
#include "SysTaskMgr/task.h"

static virtio_gpu_driver_state_t device;
static _Atomic int virtio_signalled = 0;
static _Atomic int virtio_inited = 0;
static int virtio_queue_avl = 0;

void intrpt_handler(int idx) {
    idx = 0;
    virtio_signalled = true;
    //local_spinlock_unlock(&virtio_signalled);
}

void virtio_task_handler(void *arg) {
    arg = NULL;

    while(!virtio_inited)
        ;

    while(true) {
        while(virtio_signalled) {
            local_spinlock_lock(&virtio_queue_avl);
            
            virtio_signalled = false;
            for(int i = 0; i < VIRTIO_GPU_VIRTQ_COUNT; i++) {
                virtio_accept_used(device.common_state, i);
            }

            virtio_gpu_config_t *cfg = (virtio_gpu_config_t*)device.common_state->dev_cfg;

            if(cfg->events_read & VIRTIO_GPU_EVENT_DISPLAY) {
                DEBUG_PRINT("Display resized\r\n");
                cfg->events_clear = VIRTIO_GPU_EVENT_DISPLAY;
                virtio_gpu_getdisplayinfo(virtio_gpu_displayinit_handler);
                virtio_notify(device.common_state, 0);
            }

            local_spinlock_unlock(&virtio_queue_avl);
        }
    }
}

void virtio_gpu_submitcmd(int q, void *cmd, int cmd_len, void *resp, int resp_len, void (*resp_handler)(virtio_virtq_cmd_state_t*)) {
    virtio_postcmd(device.common_state, q, cmd, cmd_len, resp, resp_len, resp_handler);
}

void virtio_gpu_default_handler(virtio_virtq_cmd_state_t *cmd) {
    //free descriptors and print any error codes
    virtio_gpu_ctrl_hdr_t *resp = (virtio_gpu_ctrl_hdr_t *)cmd->resp.virt;
    if(resp->type >= 0x1200 && resp->type <= 0x1205) {
        DEBUG_PRINT("VIRTIO_GPU: ERROR\r\n");
    }

    free(cmd->cmd.virt);
    free(cmd->resp.virt);
}

void virtio_gpu_getdisplayinfo(void (*handler)(virtio_virtq_cmd_state_t*)) {
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

void virtio_gpu_create2d(uint32_t id, virtio_gpu_formats_t fmt, uint32_t width, uint32_t height) {
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

void virtio_gpu_unref(uint32_t rsc_id) {
    virtio_gpu_resource_unref_t *rsc_d = malloc(sizeof(virtio_gpu_resource_unref_t));
    virtio_gpu_ctrl_hdr_t *resp = malloc(sizeof(virtio_gpu_ctrl_hdr_t));

    rsc_d->hdr.type = VIRTIO_GPU_CMD_RESOURCE_UNREF;
    rsc_d->hdr.flags = 0;
    rsc_d->hdr.fence_id = 0;
    rsc_d->hdr.ctx_id = 0;
    rsc_d->hdr.padding = 0;

    rsc_d->resource_id = rsc_id;
    rsc_d->padding = 0;

    memset(resp, 0, sizeof(virtio_gpu_ctrl_hdr_t));

    virtio_gpu_submitcmd(0, rsc_d, sizeof(virtio_gpu_resource_unref_t), resp, sizeof(virtio_gpu_ctrl_hdr_t), virtio_gpu_default_handler);
}

void virtio_gpu_setscanout(uint32_t scanout_id, uint32_t resource_id, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
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

void virtio_gpu_flush(uint32_t resource_id, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
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

void virtio_gpu_transfertohost2d(uint32_t resource_id, uint64_t offset, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
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

void virtio_gpu_detachbacking(uint32_t rsc_id) {
    virtio_gpu_resource_detach_backing_t *rsc_d = malloc(sizeof(virtio_gpu_resource_detach_backing_t));
    virtio_gpu_ctrl_hdr_t *resp = malloc(sizeof(virtio_gpu_ctrl_hdr_t));

    rsc_d->hdr.type = VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING;
    rsc_d->hdr.flags = 0;
    rsc_d->hdr.fence_id = 0;
    rsc_d->hdr.ctx_id = 0;
    rsc_d->hdr.padding = 0;

    rsc_d->resource_id = rsc_id;
    rsc_d->padding = 0;

    memset(resp, 0, sizeof(virtio_gpu_ctrl_hdr_t));

    virtio_gpu_submitcmd(0, rsc_d, sizeof(virtio_gpu_resource_detach_backing_t), resp, sizeof(virtio_gpu_ctrl_hdr_t), virtio_gpu_default_handler);
}

//3d acceleration commands
void virtio_gpu_ctx_create(uint32_t ctx_id, char name[64]) {
    virtio_gpu_ctx_create_t *rsc_d = malloc(sizeof(virtio_gpu_ctx_create_t));
    virtio_gpu_ctrl_hdr_t *resp = malloc(sizeof(virtio_gpu_ctrl_hdr_t));

    rsc_d->hdr.type = VIRTIO_GPU_CMD_CTX_CREATE;
    rsc_d->hdr.flags = 0;
    rsc_d->hdr.fence_id = 0;
    rsc_d->hdr.ctx_id = ctx_id;
    rsc_d->hdr.padding = 0;

    rsc_d->nlen = strnlen(name, 64);
    rsc_d->padding = 0;
    strncpy(rsc_d->debug_name, name, 64);

    memset(resp, 0, sizeof(virtio_gpu_ctrl_hdr_t));

    virtio_gpu_submitcmd(0, rsc_d, sizeof(virtio_gpu_ctx_create_t), resp, sizeof(virtio_gpu_ctrl_hdr_t), virtio_gpu_default_handler);
}

void virtio_gpu_ctx_destroy(uint32_t ctx_id) {
    virtio_gpu_ctrl_hdr_t *rsc_d = malloc(sizeof(virtio_gpu_ctrl_hdr_t));
    virtio_gpu_ctrl_hdr_t *resp = malloc(sizeof(virtio_gpu_ctrl_hdr_t));

    rsc_d->type = VIRTIO_GPU_CMD_CTX_DESTROY;
    rsc_d->flags = 0;
    rsc_d->fence_id = 0;
    rsc_d->ctx_id = ctx_id;
    rsc_d->padding = 0;

    memset(resp, 0, sizeof(virtio_gpu_ctrl_hdr_t));

    virtio_gpu_submitcmd(0, rsc_d, sizeof(virtio_gpu_ctrl_hdr_t), resp, sizeof(virtio_gpu_ctrl_hdr_t), virtio_gpu_default_handler);
}

void virtio_gpu_ctx_attachresource(uint32_t ctx_id, uint32_t handle) {
    virtio_gpu_ctx_attach_resource_t *rsc_d = malloc(sizeof(virtio_gpu_ctx_attach_resource_t));
    virtio_gpu_ctrl_hdr_t *resp = malloc(sizeof(virtio_gpu_ctrl_hdr_t));

    rsc_d->hdr.type = VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE;
    rsc_d->hdr.flags = 0;
    rsc_d->hdr.fence_id = 0;
    rsc_d->hdr.ctx_id = ctx_id;
    rsc_d->hdr.padding = 0;

    rsc_d->handle = handle;
    rsc_d->padding = 0;

    memset(resp, 0, sizeof(virtio_gpu_ctrl_hdr_t));

    virtio_gpu_submitcmd(0, rsc_d, sizeof(virtio_gpu_ctx_attach_resource_t), resp, sizeof(virtio_gpu_ctrl_hdr_t), virtio_gpu_default_handler);
}

void virtio_gpu_ctx_submit(uint32_t ctx_id, uint32_t *cmds, uint32_t sz) {
    virtio_gpu_submit_t *rsc_d = malloc(sizeof(virtio_gpu_submit_t));
    virtio_gpu_ctrl_hdr_t *resp = malloc(sizeof(virtio_gpu_ctrl_hdr_t));

    rsc_d->hdr.type = VIRTIO_GPU_CMD_SUBMIT_3D;
    rsc_d->hdr.flags = 0;
    rsc_d->hdr.fence_id = 0;
    rsc_d->hdr.ctx_id = ctx_id;
    rsc_d->hdr.padding = 0;

    rsc_d->size = sz;
    rsc_d->padding = 0;
    memcpy(rsc_d->buffer, cmds, sz * sizeof(uint32_t));

    memset(resp, 0, sizeof(virtio_gpu_ctrl_hdr_t));

    virtio_gpu_submitcmd(0, rsc_d, sizeof(virtio_gpu_submit_t) + sz * sizeof(uint32_t), resp, sizeof(virtio_gpu_ctrl_hdr_t), virtio_gpu_default_handler);
}

void virtio_gpu_displayinit_handler(virtio_virtq_cmd_state_t *cmd) {

    //read the display info, initialize all enabled scanouts
    virtio_gpu_resp_display_info_t *display_info = (virtio_gpu_resp_display_info_t*)cmd->resp.virt;

    for(int i = 0; i < VIRTIO_GPU_MAX_SCANOUTS; i++) {
        if(display_info->pmodes[i].enabled) {

            //free the previous resource_id
            if(device.scanouts[i].resource_id != 0) {

                DEBUG_PRINT("Deleting previous framebuffer.\r\n");

                virtio_gpu_detachbacking(device.scanouts[i].resource_id);
                virtio_gpu_unref(device.scanouts[i].resource_id);
                virtio_notify(device.common_state, 0);

                size_t sz = device.scanouts[i].w * device.scanouts[i].h * sizeof(uint32_t);
                if(sz % KiB(4))
                    sz += KiB(4) - (sz % KiB(4));
                pagealloc_free(device.scanouts[i].phys_addr, sz);
            }

            //setup this scanout
            //create a 2d resource
            uint32_t n_res_id = device.resource_ids++;
            virtio_gpu_create2d(n_res_id, VIRTIO_GPU_FORMAT_X8R8G8B8_UNORM, display_info->pmodes[i].r.width, display_info->pmodes[i].r.height);

            //set its backing data
            size_t sz = display_info->pmodes[i].r.width * display_info->pmodes[i].r.height * sizeof(uint32_t);
            uintptr_t fbuf = pagealloc_alloc(0, 0, physmem_alloc_flags_data, sz);
            virtio_gpu_attachbacking(n_res_id, fbuf, sz);

            //set scanout
            virtio_gpu_setscanout(i, n_res_id, 0, 0, display_info->pmodes[i].r.width, display_info->pmodes[i].r.height);

            uint32_t *vaddr = (uint32_t*)vmem_phystovirt(fbuf, sz, vmem_flags_uncached | vmem_flags_kernel | vmem_flags_rw);
            memset(vaddr, 0xff, sz);

            virtio_gpu_transfertohost2d(n_res_id, 0, 0, 0, display_info->pmodes[i].r.width, display_info->pmodes[i].r.height);
            virtio_gpu_flush(n_res_id, 0, 0, display_info->pmodes[i].r.width, display_info->pmodes[i].r.height);

            virtio_notify(device.common_state, 0);

            device.scanouts[i].x = display_info->pmodes[i].r.x * 0;
            device.scanouts[i].y = display_info->pmodes[i].r.y * 0;
            device.scanouts[i].w = display_info->pmodes[i].r.width;
            device.scanouts[i].h = display_info->pmodes[i].r.height;
            device.scanouts[i].phys_addr = fbuf;
            device.scanouts[i].virt_addr = vaddr;
            device.scanouts[i].resource_id = n_res_id;
        }
    }

    virtio_gpu_default_handler(cmd);
}

//TODO: Implement Display driver API
//TODO: design Graphics driver API, filling command buffers in user space to submit to the driver

int module_init(void *ecam) {


    memset(&device, 0, sizeof(device));

    cs_id ss_id = 0;
    cs_error ss_err = create_task_kernel(cs_task_type_process, "virtio_gpu_0", task_permissions_kernel, &ss_id);
    DEBUG_PRINT("VirtioGpu initializing...\r\n");
    if(ss_err != CS_OK)
        PANIC("VIRTIO_ERR0");
    ss_err = start_task_kernel(ss_id, virtio_task_handler);
    if(ss_err != CS_OK)
        PANIC("VIRTIO_ERR1");

    device.qstate[0] = device.controlq;
    device.qstate[1] = device.cursorq;
    device.common_state = virtio_initialize(ecam, intrpt_handler, device.qstate, device.avail_idx, device.used_idx);

    device.resource_ids = 1;

    uint32_t features = virtio_getfeatures(device.common_state, 0);

    device.virgl_mode = false;
    if(features & VIRTIO_GPU_F_VIRGL) {
        DEBUG_PRINT("VirGL Support Enabled.\r\n");
        device.virgl_mode = true;
        virtio_setfeatures(device.common_state, VIRTIO_GPU_F_VIRGL | VIRTIO_F_INDIRECT_DESC, 0);
    } else
        virtio_setfeatures(device.common_state, VIRTIO_F_INDIRECT_DESC, 0);

    if(!virtio_features_ok(device.common_state))
        return -1;

    //setup virtqueues
    virtio_setupqueue(device.common_state, 0, VIRTIO_GPU_VIRTQ_LEN);
    virtio_setupqueue(device.common_state, 1, VIRTIO_GPU_VIRTQ_LEN);

    virtio_driver_ok(device.common_state);

    virtio_gpu_getdisplayinfo(virtio_gpu_displayinit_handler);
    virtio_notify(device.common_state, 0);

    virtio_inited = true;

    return 0;
}