// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINALSEMI_CORENETWORK_ICMP_H
#define CARDINALSEMI_CORENETWORK_ICMP_H

#include <stdint.h>
#include <types.h>

#include "net_priv.h"
#include "ip.h"

#define ICMP_TYPE_ECHO_REPLY (0)
#define ICMP_TYPE_ECHO_REQUEST (8)

typedef struct {
    uint8_t type;
    uint8_t code;
    uint16_t csum;
    uint8_t body[0];  // type-specific (for echo: 16-bit id, 16-bit seq, then data)
} PACKED icmp_t;

int icmp_ipv4_rx(interface_def_t *interface, const uint8_t *src_mac, ipv4_t *packet);

#endif
