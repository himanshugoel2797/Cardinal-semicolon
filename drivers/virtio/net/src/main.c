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
#include "net.h"

#include "SysVirtualMemory/vmem.h"
#include "SysPhysicalMemory/phys_mem.h"
#include "SysTaskMgr/task.h"

static virtio_net_driver_state_t device;
static _Atomic int virtio_signalled = 0;
static _Atomic int virtio_inited = 0;
static int virtio_queue_avl = 0;

static void intrpt_handler(int idx) {
    idx = 0;
    DEBUG_PRINT("VirtioNet Interrupt!");
    virtio_signalled = true;
    //local_spinlock_unlock(&virtio_signalled);
}

static void virtio_task_handler(void *arg) {
    arg = NULL;

    while(!virtio_inited)
        ;

    while(true) {
        while(virtio_signalled) {
            local_spinlock_lock(&virtio_queue_avl);

            virtio_signalled = false;
            
            local_spinlock_unlock(&virtio_queue_avl);
        }
    }
}

int virtio_net_sendpacket(void *packet, int len, int gso){

    return 0;
}

int virtio_net_linkstatus(){
    return device.cfg->status == VIRTIO_NET_S_LINK_UP;
}

int module_init(void *ecam) {

    memset(&device, 0, sizeof(device));

    cs_id ss_id = 0;
    cs_error ss_err = create_task_kernel(cs_task_type_process, "virtio_net_0", task_permissions_kernel, &ss_id);
    DEBUG_PRINT("VirtioNet initializing...\r\n");
    if(ss_err != CS_OK)
        PANIC("VIRTIO_NET_ERR0");
    ss_err = start_task_kernel(ss_id, virtio_task_handler);
    if(ss_err != CS_OK)
        PANIC("VIRTIO_NET_ERR1");

    for(int i = 0; i < VIRTIO_NET_QUEUE_CNT; i++){
        device.qstate[i] = malloc(sizeof(virtio_virtq_cmd_state_t) * VIRTIO_NET_QUEUE_LEN);
        device.avail_idx[i] = 0;
        device.used_idx[i] = 0;
    }
    device.common_state = virtio_initialize(ecam, intrpt_handler, device.qstate, device.avail_idx, device.used_idx);
    device.cfg = (virtio_net_cfg_t*)device.common_state->dev_cfg;

    uint32_t features = virtio_getfeatures(device.common_state, 0);

    if(features & VIRTIO_NET_F_CSUM){
        device.checksum_offload = true;
    }

    virtio_setfeatures(device.common_state, VIRTIO_NET_F_MAC | (device.checksum_offload ? VIRTIO_NET_F_CSUM : 0), 0);

    if(!virtio_features_ok(device.common_state))
        return -1;

    //setup virtqueues
    for(int i = 0; i < VIRTIO_NET_QUEUE_CNT; i++)
        virtio_setupqueue(device.common_state, 0, VIRTIO_NET_QUEUE_LEN);

    virtio_driver_ok(device.common_state);

    

    virtio_inited = true;

    return 0;
}