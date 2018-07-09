// Copyright (c) 2018 Himanshu Goel
// 
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINAL_SEMI_VIRTIO_NET_H
#define CARDINAL_SEMI_VIRTIO_NET_H

#include "virtio.h"

#define VIRTIO_NET_QUEUE_CNT 2
#define VIRTIO_NET_QUEUE_LEN 256

#define VIRTIO_NET_F_CSUM (1 << 0)
#define VIRTIO_NET_F_MAC (1 << 5)

#define VIRTIO_NET_S_LINK_UP 1
typedef struct {
    uint8_t mac[6];
    uint16_t status;
    uint16_t virtq_pairs;
} virtio_net_cfg_t;

typedef struct {
    #define VIRTIO_NET_HDR_F_NEEDS_CSUM 1
    uint8_t flags;
    #define VIRTIO_NET_GSO_NONE 0
    #define VIRTIO_NET_GSO_TCPV4 1
    #define VIRTIO_NET_GSO_UDP 3
    #define VIRTIO_NET_GSO_TCPV6 4
    #define VIRTIO_NET_GSO_ECN 0x80
    uint8_t gso_type;
    uint16_t hdr_len;
    uint16_t gso_sz;
    uint16_t csum_start;
    uint16_t csum_offset;
    uint16_t num_buffers;
} virtio_net_cmd_hdr_t;

typedef struct {
    virtio_state_t *common_state;
    virtio_net_cfg_t *cfg;

    virtio_virtq_cmd_state_t *qstate[VIRTIO_NET_QUEUE_CNT];
    int avail_idx[VIRTIO_NET_QUEUE_CNT];
    int used_idx[VIRTIO_NET_QUEUE_CNT];
    bool checksum_offload;
} virtio_net_driver_state_t;

#endif