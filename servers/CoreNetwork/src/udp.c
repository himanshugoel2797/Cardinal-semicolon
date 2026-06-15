/**
 * Copyright (c) 2018 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

#include <stdlib.h>
#include <string.h>

#include "udp.h"
#include "checksum.h"

// UDP checksum covers a pseudo-header + the UDP segment (header, including the
// stored checksum field, + payload). A valid packet folds to zero, so there is
// no need to zero/restore the checksum field -- the packet is never mutated.
//
// All multi-byte IP/UDP fields sit in network byte order in memory, and
// net_csum_acc reads raw bytes; the internet-checksum byte-order property
// (RFC 1071) makes the fold-to-zero check endianness-independent. `avail` is the
// number of segment bytes actually received, used to clamp the on-wire length so
// a lying udp->len cannot drive an out-of-bounds read.

static int udp_seg_len(const udp_t *udp, int avail) {
    if (avail < (int)sizeof(udp_t))  // fewer bytes than the header -> can't even
        return -1;                   // read udp->len without going out of bounds
    int seg_len = (int)TO_LE_FRM_BE_16(udp->len);
    if (seg_len < (int)sizeof(udp_t))  // shorter than the UDP header -> malformed
        return -1;
    if (seg_len > avail)
        seg_len = avail;
    return seg_len;
}

static int udp_ipv4_csum_ok(const ipv4_t *packet, const udp_t *udp, int seg_len) {
    uint8_t ph[12];
    memcpy(ph + 0, &packet->src_ip, 4);   // network order in memory
    memcpy(ph + 4, &packet->dst_ip, 4);
    ph[8] = 0;
    ph[9] = IP_PROTOCOL_UDP;
    memcpy(ph + 10, &udp->len, 2);        // network order on-wire length
    uint32_t sum = net_csum_acc(0, ph, sizeof(ph));
    sum = net_csum_acc(sum, udp, seg_len);
    return net_csum_fold(sum) == 0;
}

int udp_ipv4_rx(interface_def_t *interface, ipv4_t *packet, int len) {
    udp_t *udp = (udp_t *)packet->body;
    int seg_len = udp_seg_len(udp, len);
    if (seg_len < 0)
        return 0;  // malformed -> drop

    // A transmitted checksum of 0 means "no checksum" for UDP-over-IPv4.
    if (udp->csum == 0 || udp_ipv4_csum_ok(packet, udp, seg_len)) {
        //DEBUG_PRINT("UDP!!\r\n");
        //TODO: Interface for services to subscribe to ports
        //  From here, the packet gets queued into the destination udp port, if present
        //  If not present, the packet is dropped
        //TODO: Setup tx infrastructure:
        //  CoreNetwork has a separate tx queue per device
        //  Has a tx thread which handles pushing packets out to the driver
    }

    interface = NULL;
    return 0;
}

static int udp_ipv6_csum_ok(const ipv6_t *packet, const udp_t *udp, int seg_len) {
    uint8_t ph[40];
    memcpy(ph + 0, packet->src_ip, 16);
    memcpy(ph + 16, packet->dst_ip, 16);
    uint32_t ulen_be = TO_BE_FRM_LE_32((uint32_t)seg_len);
    memcpy(ph + 32, &ulen_be, 4);         // upper-layer length, network order
    ph[36] = ph[37] = ph[38] = 0;
    ph[39] = IP_PROTOCOL_UDP;
    uint32_t sum = net_csum_acc(0, ph, sizeof(ph));
    sum = net_csum_acc(sum, udp, seg_len);
    return net_csum_fold(sum) == 0;
}

int udp_ipv6_rx(interface_def_t *interface, ipv6_t *packet, int len) {
    udp_t *udp = (udp_t *)packet->body;
    int seg_len = udp_seg_len(udp, len);
    if (seg_len < 0)
        return 0;  // malformed -> drop

    // Unlike IPv4, the UDP checksum is mandatory over IPv6.
    if (udp_ipv6_csum_ok(packet, udp, seg_len)) {
        DEBUG_PRINT("UDPv6!!\r\n");
        //From here, the packet gets queued into the destination udp port, if present
        //If not present, the packet is dropped
    }

    interface = NULL;
    return 0;
}
