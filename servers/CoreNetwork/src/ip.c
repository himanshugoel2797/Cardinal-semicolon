/**
 * Copyright (c) 2018 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

#include <stdlib.h>
#include <string.h>

#include "ip.h"
#include "udp.h"
#include "arp.h"
#include "icmp.h"
#include "ethernet.h"
#include "checksum.h"

// IPv4 header checksum: valid headers fold to zero. Covers ihl*4 bytes so the
// (currently unused) options area is still summed if present.
static uint16_t ipv4_verify_csum(ipv4_t *packet) {
    return net_checksum16(packet, packet->ihl * 4);
}

int ipv4_rx(interface_def_t *interface, const uint8_t *src_mac, void *packet, int len) {
    ipv4_t *ip_pack = (ipv4_t*)packet;

    if (ip_pack->version != 4 || ip_pack->ihl < 5)
        return 0;

    if(ipv4_verify_csum(ip_pack) == 0) {
        // Learn the sender's L2/L3 mapping from any valid traffic so replies do
        // not depend on a prior ARP exchange being cached.
        arp_cache_update(ip_pack->src_ip, src_mac);

        if (ip_pack->protocol == IP_PROTOCOL_ICMP) {
            icmp_ipv4_rx(interface, src_mac, ip_pack);
        } else if (ip_pack->protocol == IP_PROTOCOL_TCP) {
            //TODO: Forward to TCP layer (see notes/servers/CoreNetwork.md)
        } else if (ip_pack->protocol == IP_PROTOCOL_UDP) {
            //Forward to UDP layer
            udp_ipv4_rx(interface, ip_pack, len - sizeof(ipv4_t));
        } else {
            //TODO: Queue this packet into the raw queue, for potential user mode processing
        }
    }

    return 0;
}

int ipv4_tx(interface_def_t *interface, uint32_t dst_ip, const uint8_t *dst_mac,
            uint8_t protocol, const void *payload, int payload_len) {
    if (payload_len < 0)
        return -1;

    int total_len = sizeof(ipv4_t) + payload_len;
    uint8_t *buf = (uint8_t *)malloc(total_len);
    if (buf == NULL)
        return -1;

    ipv4_t *ip = (ipv4_t *)buf;
    memset(ip, 0, sizeof(ipv4_t));
    ip->version = 4;
    ip->ihl = 5;
    ip->total_len = TO_BE_FRM_LE_16((uint16_t)total_len);
    ip->ident = 0;
    ip->flags = 0;
    ip->fragment_off = 0;
    ip->ttl = 64;
    ip->protocol = protocol;
    ip->src_ip = interface->ip;
    ip->dst_ip = dst_ip;
    ip->hdr_csum = 0;
    ip->hdr_csum = net_checksum16(ip, sizeof(ipv4_t));

    memcpy(ip->body, payload, payload_len);

    int ret = ethernet_tx(interface, dst_mac, ETHERNET_TYPE_IPv4, buf, total_len);

    free(buf);
    return ret;
}

int ipv6_rx(interface_def_t *interface, const uint8_t *src_mac, void *packet, int len) {
    ipv6_t *ip_pack = (ipv6_t*)packet;
    src_mac = NULL;

    if (ip_pack->protocol == IP_PROTOCOL_ICMP) {
        //TODO: Forward to ICMPv6 layer (see notes/servers/CoreNetwork.md)
    } else if (ip_pack->protocol == IP_PROTOCOL_TCP) {
        //TODO: Forward to TCP layer
    } else if (ip_pack->protocol == IP_PROTOCOL_UDP) {
        //Forward to UDP layer
        udp_ipv6_rx(interface, ip_pack, len - sizeof(ipv6_t));
    } else {
        //TODO: Queue this packet into the raw queue, for potential user mode processing
    }

    return 0;
}
