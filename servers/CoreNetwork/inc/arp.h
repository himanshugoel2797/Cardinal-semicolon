// Copyright (c) 2018 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINALSEMI_CORENETWORK_ARP_H
#define CARDINALSEMI_CORENETWORK_ARP_H

#include <stdint.h>
#include <types.h>

#include "net_priv.h"

typedef struct {
    uint16_t hw_type;
    uint16_t protocol_type;
    uint8_t hw_addr_len;
    uint8_t protocol_addr_len;
    uint16_t opcode;
    uint8_t src_mac[6];
    uint8_t src_ip[4];
    uint8_t dst_mac[6];
    uint8_t dst_ip[4];
} PACKED arp_t;

int arp_rx(interface_def_t *interface, void *packet, int len);

#endif