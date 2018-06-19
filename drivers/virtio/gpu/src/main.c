/**
 * Copyright (c) 2018 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

#include "virtio.h"
#include "virtio_gpu.h"

#include <stdlib.h>
#include <string.h>

static virtio_gpu_driver_state_t device;

void intrpt_handler(int idx) {
    idx = 0;

    for(int i = 0; i < VIRTIO_GPU_VIRTQ_COUNT; i++)
        virtio_accept_used(device.common_state, i);
}

void virtio_gpu_submitcmd(int q, void *cmd, int cmd_len, void *resp, int resp_len, void (*resp_handler)(virtio_virtq_cmd_state_t*))
{
    int cmd_idx = virtio_postcmd(device.common_state, q, cmd, cmd_len, resp, resp_len, resp_handler);
}

void virtio_gpu_getdisplayinfo(void (*handler)(virtio_virtq_cmd_state_t*))
{
    static virtio_gpu_ctrl_hdr_t display_info_cmd;
    static virtio_gpu_resp_display_info_t display_info_resp;
    
    //Retrieve display info
    display_info_cmd.type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO;
    display_info_cmd.flags = 0;
    display_info_cmd.fence_id = 0;
    display_info_cmd.ctx_id = 0;
    display_info_cmd.padding = 0;

    memset(&display_info_resp, 0, sizeof(display_info_resp));

    virtio_gpu_submitcmd(0, &display_info_cmd, sizeof(display_info_cmd), &display_info_resp, sizeof(display_info_resp), handler);
}

/*void virtio_gpu_create2d(uint32_t id, virtio_gpu_formats_t fmt, uint32_t width, uint32_t height)
{

}*/

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

    virtio_gpu_getdisplayinfo(NULL);
    virtio_notify(device.common_state, 0);

    while(true) {
    }

    return 0;
}