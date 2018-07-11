// Copyright (c) 2018 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINALSEMI_CORENETWORK_IP_H
#define CARDINALSEMI_CORENETWORK_IP_H

#include <stdint.h>
#include <types.h>

#include "net_priv.h"

#define IP_PROTOCOL_ICMP (1)
#define IP_PROTOCOL_TCP (6)
#define IP_PROTOCOL_UDP (17)

typedef struct {
    uint8_t version : 4;    //Value: 4
    uint8_t ihl : 4;        //Only accept value = 5
    uint8_t dscp : 6;       //TODO:
    uint8_t ecn : 2;        //Congestion Notification - Not supported
    uint16_t total_len;
    uint16_t ident;
    uint16_t flags : 3;
    uint16_t fragment_off : 13;
    uint8_t ttl;
    uint8_t protocol;
    uint16_t hdr_csum;
    uint8_t src_ip[4];
    uint8_t dst_ip[4];
    uint8_t body[0];
} ipv4_t;

typedef struct {
    uint32_t version_traffic_flow;
    uint16_t payload_len;
    uint8_t protocol;
    uint8_t ttl;
    uint8_t src_ip [16];
    uint8_t dst_ip [16];
    uint8_t body[0];
} ipv6_t;

int ipv4_rx(interface_def_t *interface, void *packet, int len);
int ipv6_rx(interface_def_t *interface, void *packet, int len);

#endif