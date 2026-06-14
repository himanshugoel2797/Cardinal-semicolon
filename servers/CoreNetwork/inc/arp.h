// Copyright (c) 2018 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINALSEMI_CORENETWORK_ARP_H
#define CARDINALSEMI_CORENETWORK_ARP_H

#include <stdint.h>
#include <types.h>

#include "net_priv.h"

#define ARP_HW_TYPE_ETHERNET (1)
#define ARP_PROTO_TYPE_IPV4 (0x0800)

#define ARP_REQUEST 0x0001
#define ARP_REPLY 0x0002

typedef struct {
    uint16_t hw_type;
    uint16_t protocol_type;
    uint8_t hw_addr_len;
    uint8_t protocol_addr_len;
    uint16_t opcode;
    uint8_t src_mac[6];
    uint32_t src_ip;
    uint8_t dst_mac[6];
    uint32_t dst_ip;
} PACKED arp_t;

int arp_rx(interface_def_t *interface, const uint8_t *src_mac, void *packet, int len);

// Insert or refresh an IPv4 (network-order) -> MAC mapping in the ARP cache.
void arp_cache_update(uint32_t ip, const uint8_t *mac);

// Look up the MAC for an IPv4 address (network order). Writes 6 bytes into
// `mac_out` and returns true on hit, false on miss.
bool arp_cache_lookup(uint32_t ip, uint8_t *mac_out);

#endif
