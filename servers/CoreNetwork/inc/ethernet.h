// Copyright (c) 2018 Himanshu Goel
// 
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINALSEMI_ETHERNET_DEF_H
#define CARDINALSEMI_ETHERNET_DEF_H

#include <stdint.h>
#include <types.h>

#define ETHERNET_TYPE_ARP (TO_BE_16(0x0806))
#define ETHERNET_TYPE_IPv4 (TO_BE_16(0x0800))
#define ETHERNET_TYPE_IPv6 (TO_BE_16(0x86DD))

typedef struct {
    uint8_t dst_mac[6];
    uint8_t src_mac[6];
    uint16_t type;
    uint8_t body[0];
} PACKED ethernet_frame_t;

#endif