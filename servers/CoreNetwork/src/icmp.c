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

int icmp_ipv4_rx(interface_def_t *interface, const uint8_t *src_mac, ipv4_t *packet,
                 int hdr_len, int icmp_len) {
    // Only handle datagrams addressed to this interface.
    if (packet->dst_ip != interface->ip)
        return 0;

    if (icmp_len < (int)sizeof(icmp_t))
        return 0;

    // The ICMP message begins after the IPv4 header (hdr_len, which accounts for
    // options when ihl > 5), not at a fixed offset. The caller has validated
    // that hdr_len + icmp_len lies within the received frame.
    icmp_t *icmp = (icmp_t *)((uint8_t *)packet + hdr_len);

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
