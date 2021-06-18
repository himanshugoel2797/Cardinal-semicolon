/**
 * Copyright (c) 2018 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

#include <stdlib.h>

#include "udp.h"

static uint16_t udp_ipv4_verify_csum(ipv4_t *packet, udp_t *udp) {

    uint16_t csum_bkp = udp->csum;
    udp->csum = 0;

    uint32_t csum = 0;
    uint8_t *udp_u8 = (uint8_t*)udp;
    uint16_t *packet_ptr = (uint16_t*)udp;

    uint32_t src_ip = (packet->src_ip);
    uint32_t dst_ip = (packet->dst_ip);

    uint16_t *src_ip_a = (uint16_t *)&src_ip;
    uint16_t *dst_ip_a = (uint16_t *)&dst_ip;

    csum += (src_ip >> 16) & 0xffff;
    csum += (src_ip) & 0xffff;
    csum += (dst_ip >> 16) & 0xffff;
    csum += (dst_ip) & 0xffff;
    csum += ((uint16_t)packet->protocol) << 8;

    csum += udp->len;
    
    uint32_t udp_len = TO_LE_FRM_BE_16(udp->len);
    while (udp_len > 1){
        csum += *packet_ptr++;
        udp_len -= 2;
    }

    if(udp_len > 0)
        csum += *packet_ptr & 0x00ff;

    while(csum > 0xffff)
        csum = (csum & 0xffff) + (csum >> 16);

    return !(((uint16_t)~csum) == csum_bkp);
}

int udp_ipv4_rx(interface_def_t *interface, ipv4_t *packet, int len) {
    udp_t *udp = (udp_t*)packet->body;

    if((udp_ipv4_verify_csum(packet, udp) == 0) | (udp->csum == 0)) {
        DEBUG_PRINT("UDP!!\r\n");
        //TODO: Interface for services to subscribe to ports
        //  From here, the packet gets queued into the destination udp port, if present
        //  If not present, the packet is dropped
        //TODO: Setup tx infrastructure:
        //  CoreNetwork has a separate tx queue per device
        //  Has a tx thread which handles pushing packets out to the driver
    }

    interface = NULL;
    len = 0;

    return 0;
}

static uint16_t udp_ipv6_verify_csum(ipv6_t *packet, udp_t *udp) {

    uint32_t csum = 0;
    uint8_t *udp_u8 = (uint8_t*)udp;
    uint16_t *packet_ptr = (uint16_t*)udp;

    uint16_t *pseudo_ip = (uint16_t*)packet->src_ip;

    csum += packet->protocol;
    for(uint32_t i = 0; i < 16; i++)
        csum += TO_LE_FRM_BE_16(pseudo_ip[i]);

    for(uint32_t i = 0; i < udp->len / 2; i++)
        csum += TO_LE_FRM_BE_16(packet_ptr[i]);

    if(udp->len & 1)
        csum += udp_u8[udp->len - 1];

    csum += TO_LE_FRM_BE_16(udp->len);

    while(csum > 0xffff)
        csum = (csum & 0xffff) + (csum >> 16);

    return ~csum;
}

int udp_ipv6_rx(interface_def_t *interface, ipv6_t *packet, int len) {
    udp_t *udp = (udp_t*)packet->body;

    if((udp_ipv6_verify_csum(packet, udp) == 0) | (udp->csum == 0)) {
        DEBUG_PRINT("UDPv6!!\r\n");
        //From here, the packet gets queued into the destination udp port, if present
        //If not present, the packet is dropped
    }

    interface = NULL;
    len = 0;

    return 0;
}