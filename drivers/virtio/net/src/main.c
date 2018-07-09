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

            //acknowledge transmitted packets

            //forward received packets

            virtio_signalled = false;
            
            local_spinlock_unlock(&virtio_queue_avl);
        }
    }
}

int virtio_net_sendpacket(void *packet, int len, int gso){
    gso = 0;

    int ent_idx = device.avail_idx[VIRTIO_NET_Q_TX];
    uint8_t *data_buf = device.tx_buf_virt + ent_idx * KiB(2);

    memcpy(data_buf + sizeof(virtio_net_cmd_hdr_t), packet, len);
    virtio_net_cmd_hdr_t *hdr = (virtio_net_cmd_hdr_t*)data_buf;

    hdr->flags = 0;
    hdr->gso_type = VIRTIO_NET_GSO_NONE;
    hdr->hdr_len = sizeof(virtio_net_cmd_hdr_t);
    hdr->gso_sz = len;
    hdr->csum_start = 0;
    hdr->csum_offset = 0;
    hdr->num_buffers = 0;

    virtio_postcmd_noresp(device.common_state, VIRTIO_NET_Q_TX, data_buf, len + sizeof(virtio_net_cmd_hdr_t), NULL);
    virtio_notify(device.common_state, VIRTIO_NET_Q_TX);
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

    virtio_setfeatures(device.common_state, VIRTIO_NET_F_STATUS | VIRTIO_NET_F_MAC | (device.checksum_offload ? VIRTIO_NET_F_CSUM : 0), 0);

    if(!virtio_features_ok(device.common_state))
        return -1;

    //setup virtqueues
    for(int i = 0; i < VIRTIO_NET_QUEUE_CNT; i++)
        virtio_setupqueue(device.common_state, i, VIRTIO_NET_QUEUE_LEN);

    virtio_driver_ok(device.common_state);

    //Fill the receive virtq with buffers
    uintptr_t rcv_buf = pagealloc_alloc(0, 0, physmem_alloc_flags_data, VIRTIO_NET_QUEUE_LEN * KiB(2));
    uint8_t *rcv_buf_virt = (uint8_t*)vmem_phystovirt((intptr_t)rcv_buf, VIRTIO_NET_QUEUE_LEN * KiB(2), vmem_flags_uncached | vmem_flags_rw | vmem_flags_kernel);
    device.rx_buf_phys = rcv_buf;

    for(int i = 0; i < VIRTIO_NET_QUEUE_LEN; i++) {
        virtio_net_cmd_hdr_t *cmd_tmp = (virtio_net_cmd_hdr_t*)(rcv_buf_virt + i * KiB(2));
        virtio_addresponse(device.common_state, VIRTIO_NET_Q_RX, cmd_tmp, KiB(2));
    }

    //Allocate tx buffer
    device.tx_buf_phys = pagealloc_alloc(0, 0, physmem_alloc_flags_data, VIRTIO_NET_QUEUE_LEN * KiB(2));
    device.tx_buf_virt = (uint8_t*)vmem_phystovirt((intptr_t)device.tx_buf_phys, VIRTIO_NET_QUEUE_LEN * KiB(2), vmem_flags_uncached | vmem_flags_rw | vmem_flags_kernel);
    
    //Register to the network service
    if(virtio_net_linkstatus())
        DEBUG_PRINT("Network connected!\r\n");

    virtio_notify(device.common_state, VIRTIO_NET_Q_RX);
    virtio_notify(device.common_state, VIRTIO_NET_Q_TX);

    DEBUG_PRINT("Register Network Device: VirtioNet\r\n");

    virtio_inited = true;

    ARP_Packet packet_a;
    ARP_Packet *packet = (ARP_Packet*)&packet_a;

    for(int i = 0; i < 6; i++)
        packet->frame.dst_mac[i] = 0xFF;

    for(int i = 0; i < 6; i++)
        packet->frame.src_mac[i] = device.cfg->mac[i];

    packet->frame.type = 0x0608;

    packet->hw_type = 0x0100;
    packet->protocol_type = 0x0008;
    packet->hw_addr_len = 6;
    packet->protocol_addr_len = 4;
    packet->opcode = 0x0100;

    for(int i = 0; i < 6; i++)
        packet->src_mac[i] = device.cfg->mac[i];

    for(int i = 0; i < 4; i++)
        packet->src_ip[i] = 0;

    for(int i = 0; i < 6; i++)
        packet->dst_mac[i] = 0x00;

    for(int i = 0; i < 4; i++)
        packet->dst_ip[i] = 0x00;

    virtio_net_sendpacket(packet, sizeof(ARP_Packet), 0);

    return 0;
}