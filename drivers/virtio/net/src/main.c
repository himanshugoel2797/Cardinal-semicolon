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

#include "CoreNetwork/driver.h"

static virtio_net_driver_state_t device;
static _Atomic int virtio_signalled = 0;
static _Atomic int virtio_inited = 0;
static int virtio_queue_avl = 0;

static void intrpt_handler(int idx) {
    idx = 0;
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
            
            DEBUG_PRINT("VirtioNet Interrupt!\r\n");

            //acknowledge transmitted packets
            virtio_accept_used(device.common_state, VIRTIO_NET_Q_TX);

            //forward received packets
            virtio_accept_used(device.common_state, VIRTIO_NET_Q_RX);

            local_spinlock_unlock(&virtio_queue_avl);
        }
    }
}

static void virtio_net_resphandler(virtio_virtq_cmd_state_t *cmd) {
    //submit this response
    int state = cli();
    DEBUG_PRINT("Package Received!\r\n");

    virtio_net_cmd_hdr_t *resp = (virtio_net_cmd_hdr_t *)cmd->resp.virt;
    uint8_t *resp_u8 = (uint8_t*)cmd->resp.virt;
    network_rx_packet(device.handle, resp_u8 + sizeof(virtio_net_cmd_hdr_t), 1514);

    memset(resp_u8, 0, KiB(2));
    virtio_addresponse(device.common_state, VIRTIO_NET_Q_RX, resp_u8, KiB(2), virtio_net_resphandler);
    virtio_notify(device.common_state, VIRTIO_NET_Q_RX);

    sti(state);
}

int virtio_net_sendpacket(void *state, void *packet, int len, network_device_tx_flags_t gso){
    gso = 0;

    virtio_net_driver_state_t *device = (virtio_net_driver_state_t*)state;

    int ent_idx = device->avail_idx[VIRTIO_NET_Q_TX];
    uint8_t *data_buf = device->tx_buf_virt + ent_idx * KiB(2);

    memcpy(data_buf + sizeof(virtio_net_cmd_hdr_t), packet, len);
    virtio_net_cmd_hdr_t *hdr = (virtio_net_cmd_hdr_t*)data_buf;

    hdr->flags = 0;
    hdr->gso_type = VIRTIO_NET_GSO_NONE;
    hdr->hdr_len = 0;
    hdr->gso_sz = 0;
    hdr->csum_start = 0;
    hdr->csum_offset = 0;

    virtio_postcmd_noresp(device->common_state, VIRTIO_NET_Q_TX, data_buf, len + sizeof(virtio_net_cmd_hdr_t), NULL);
    virtio_notify(device->common_state, VIRTIO_NET_Q_TX);
    return 0;
}

int virtio_net_linkstatus(void *state){
    virtio_net_driver_state_t *device = (virtio_net_driver_state_t*)state;
    return device->cfg->status == VIRTIO_NET_S_LINK_UP;
}

static network_device_desc_t device_desc = {
    .name = "Virtio Net",
    .state = (void*)&device,
    .features = 0,
    .type = network_device_type_ethernet,
    .handlers.ether = {
        .tx = virtio_net_sendpacket,
        .link_status = virtio_net_linkstatus,
    },
    .spec_features.ether = 0,
};

int module_init(void *ecam) {

    memset(&device, 0, sizeof(device));

    cs_id ss_id = 0;
    cs_error ss_err = create_task_kernel(cs_task_type_process, "virtio_net_0", task_permissions_kernel, &ss_id);
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
        device_desc.features |= network_device_features_checksum_offload;
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
        virtio_addresponse(device.common_state, VIRTIO_NET_Q_RX, cmd_tmp, KiB(2), virtio_net_resphandler);
    }

    //Allocate tx buffer
    device.tx_buf_phys = pagealloc_alloc(0, 0, physmem_alloc_flags_data, VIRTIO_NET_QUEUE_LEN * KiB(2));
    device.tx_buf_virt = (uint8_t*)vmem_phystovirt((intptr_t)device.tx_buf_phys, VIRTIO_NET_QUEUE_LEN * KiB(2), vmem_flags_uncached | vmem_flags_rw | vmem_flags_kernel);
    
    //register as a network device
    for(int i = 0; i < 6; i++)
        device_desc.mac[i] = device.cfg->mac[i];

    network_register(&device_desc, &device.handle);

    virtio_notify(device.common_state, VIRTIO_NET_Q_RX);
    virtio_notify(device.common_state, VIRTIO_NET_Q_TX);

    virtio_inited = true;

    /*arp_t packet_a;
    arp_t *packet = (arp_t*)&packet_a;

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
        packet->dst_ip[i] = 0;

    //__asm__("cli\n\thlt" :: "a"(packet), "b"(sizeof(arp_t)));
    while(true){
        virtio_net_sendpacket(&device, packet, sizeof(arp_t), 0);
    }*/

    return 0;
}