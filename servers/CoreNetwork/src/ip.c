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
#include "icmp.h"
#include "ethernet.h"
#include "checksum.h"

int ipv4_rx(interface_def_t *interface, const uint8_t *src_mac, void *packet, int len) {
    if (len < (int)sizeof(ipv4_t))
        return 0;

    ipv4_t *ip_pack = (ipv4_t*)packet;
    if (ip_pack->version != 4 || ip_pack->ihl < 5)
        return 0;

    // Bound every header-derived length against the bytes actually received
    // (`len`) before reading them -- total_len/ihl come from the remote peer.
    int hdr_len = ip_pack->ihl * 4;
    int total_len = (int)TO_LE_FRM_BE_16(ip_pack->total_len);
    if (hdr_len < (int)sizeof(ipv4_t) || hdr_len > len)
        return 0;
    if (total_len < hdr_len || total_len > len)
        return 0;

    // Header checksum (covers ihl*4 bytes, now known to fit): valid -> folds to 0.
    if (net_checksum16(ip_pack, hdr_len) != 0)
        return 0;

    int payload_len = total_len - hdr_len;

    if (ip_pack->protocol == IP_PROTOCOL_ICMP) {
        icmp_ipv4_rx(interface, src_mac, ip_pack, hdr_len, payload_len);
    } else if (ip_pack->protocol == IP_PROTOCOL_TCP) {
        //TODO: Forward to TCP layer (see notes/servers/CoreNetwork.md)
    } else if (ip_pack->protocol == IP_PROTOCOL_UDP) {
        //Forward to UDP layer
        udp_ipv4_rx(interface, ip_pack, payload_len);
    } else {
        //TODO: Queue this packet into the raw queue, for potential user mode processing
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
