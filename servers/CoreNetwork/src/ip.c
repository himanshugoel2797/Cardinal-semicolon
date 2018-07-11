/**
 * Copyright (c) 2018 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

#include <stdlib.h>

#include "ip.h"
#include "udp.h"

uint16_t ipv4_verify_csum(ipv4_t *packet) {

    uint32_t csum = 0;
    uint16_t *packet_ptr = (uint16_t*)packet;
    for(uint32_t i = 0; i < sizeof(ipv4_t) / sizeof(uint16_t); i++)
        csum += TO_LE_FRM_BE_16(packet_ptr[i]);

    while(csum > 0xffff) {
        csum = (csum & 0xffff) + (csum >> 16);
    }

    return ~csum;
}

int ipv4_rx(interface_def_t *interface, void *packet, int len) {
    ipv4_t *ip_pack = (ipv4_t*)packet;

    if(ipv4_verify_csum(ip_pack) == 0) {
        if (ip_pack->protocol == IP_PROTOCOL_ICMP) {
            //TODO: Forward to ICMP layer
            DEBUG_PRINT("ICMP\r\n");
        } else if (ip_pack->protocol == IP_PROTOCOL_TCP) {
            //TODO: Forward to TCP layer
            DEBUG_PRINT("TCP\r\n");
        } else if (ip_pack->protocol == IP_PROTOCOL_UDP) {
            //Forward to UDP layer
            udp_ipv4_rx(interface, ip_pack, len - sizeof(ipv4_t));
        } else {
            //TODO: Queue this packet into the raw queue, for potential user mode processing
        }
    }

    return 0;
}

int ipv6_rx(interface_def_t *interface, void *packet, int len) {
    ipv6_t *ip_pack = (ipv6_t*)packet;

    if (ip_pack->protocol == IP_PROTOCOL_ICMP) {
        //TODO: Forward to ICMP layer
        DEBUG_PRINT("ICMPv6\r\n");
    } else if (ip_pack->protocol == IP_PROTOCOL_TCP) {
        //TODO: Forward to TCP layer
        DEBUG_PRINT("TCPv6\r\n");
    } else if (ip_pack->protocol == IP_PROTOCOL_UDP) {
        //Forward to UDP layer
        udp_ipv6_rx(interface, ip_pack, len - sizeof(ipv6_t));
    } else {
        //TODO: Queue this packet into the raw queue, for potential user mode processing
    }

    return 0;
}