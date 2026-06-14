/**
 * Copyright (c) 2026 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

#include <stdlib.h>
#include <string.h>

#include "icmp.h"
#include "ip.h"
#include "checksum.h"

int icmp_ipv4_rx(interface_def_t *interface, const uint8_t *src_mac, ipv4_t *packet) {
    // Only handle datagrams addressed to this interface.
    if (packet->dst_ip != interface->ip)
        return 0;

    // The driver does not report the true frame length, so derive the ICMP
    // message length from the IPv4 total-length field rather than a passed len.
    int total_len = (int)TO_LE_FRM_BE_16(packet->total_len);
    int hdr_len = packet->ihl * 4;
    int icmp_len = total_len - hdr_len;
    if (icmp_len < (int)sizeof(icmp_t))
        return 0;

    icmp_t *icmp = (icmp_t *)packet->body;

    if (net_checksum16(icmp, icmp_len) != 0)
        return 0;  // corrupt ICMP message

    if (icmp->type != ICMP_TYPE_ECHO_REQUEST)
        return 0;

    // Build the echo reply: identical body (id/seq/data) with the type flipped
    // and the checksum recomputed.
    uint8_t *reply = (uint8_t *)malloc(icmp_len);
    if (reply == NULL)
        return -1;

    memcpy(reply, icmp, icmp_len);
    icmp_t *reply_icmp = (icmp_t *)reply;
    reply_icmp->type = ICMP_TYPE_ECHO_REPLY;
    reply_icmp->code = 0;
    reply_icmp->csum = 0;
    reply_icmp->csum = net_checksum16(reply, icmp_len);

    int ret = ipv4_tx(interface, packet->src_ip, src_mac, IP_PROTOCOL_ICMP, reply, icmp_len);

    free(reply);
    return ret;
}
