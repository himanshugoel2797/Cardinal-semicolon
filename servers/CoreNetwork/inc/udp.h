// Copyright (c) 2018 Himanshu Goel
// 
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINALSEMI_CORENETWORK_UDP_H
#define CARDINALSEMI_CORENETWORK_UDP_H

#include <stdint.h>

#include "net_priv.h"
#include "ip.h"

typedef struct {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t len;
    uint16_t csum;
    uint8_t body[0];
} udp_t;

int udp_ipv4_rx(interface_def_t *interface, ipv4_t *packet, int len);

#endif