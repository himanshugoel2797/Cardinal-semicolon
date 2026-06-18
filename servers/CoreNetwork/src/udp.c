/**
 * Copyright (c) 2018 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

#include <stdlib.h>
#include <string.h>
#include <cardinal/local_spinlock.h>

#include "udp.h"
#include "ip.h"
#include "checksum.h"
#include "CoreNetwork/udp.h"

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

// One's-complement sum over the IPv4/UDP pseudo-header (src ip, dst ip, zero,
// protocol, on-wire UDP length). `udp_len_be` is the UDP length field in network
// byte order -- the same value the sender summed. Shared by the rx verify path
// and the tx build path so they cannot drift.
static uint32_t udp_ipv4_pseudo_sum(uint32_t src_ip_be, uint32_t dst_ip_be,
                                    uint16_t udp_len_be) {
    uint8_t ph[12];
    memcpy(ph + 0, &src_ip_be, 4);   // network order in memory
    memcpy(ph + 4, &dst_ip_be, 4);
    ph[8] = 0;
    ph[9] = IP_PROTOCOL_UDP;
    memcpy(ph + 10, &udp_len_be, 2); // network order on-wire length
    return net_csum_acc(0, ph, sizeof(ph));
}

static int udp_ipv4_csum_ok(const ipv4_t *packet, const udp_t *udp, int seg_len) {
    uint32_t sum = udp_ipv4_pseudo_sum(packet->src_ip, packet->dst_ip, udp->len);
    sum = net_csum_acc(sum, udp, seg_len);
    return net_csum_fold(sum) == 0;
}

// --- Port binding table -------------------------------------------------------
//
// A small static table mapping a destination port to a handler. Protected by a
// spinlock (like the ARP cache); a matching handler is copied out under the lock
// and the lock released before the handler runs, so a handler is free to call
// back into udp_send_to / udp_bind without self-deadlocking.

#define UDP_MAX_BINDINGS 16

typedef struct {
    uint16_t port;       // host order
    bool used;
    udp_handler_t handler;
    void *ctx;
} udp_binding_t;

static udp_binding_t udp_bindings[UDP_MAX_BINDINGS];
static int udp_bind_lock = 0;

int udp_bind(uint16_t port, udp_handler_t handler, void *ctx) {
    if (port == 0 || handler == NULL)
        return -1;

    local_spinlock_lock(&udp_bind_lock);
    int slot = -1;
    for (int i = 0; i < UDP_MAX_BINDINGS; i++) {
        if (udp_bindings[i].used && udp_bindings[i].port == port) {
            local_spinlock_unlock(&udp_bind_lock);  // already bound
            return -1;
        }
        if (!udp_bindings[i].used && slot < 0)
            slot = i;
    }
    if (slot < 0) {
        local_spinlock_unlock(&udp_bind_lock);      // table full
        return -1;
    }
    udp_bindings[slot].port = port;
    udp_bindings[slot].handler = handler;
    udp_bindings[slot].ctx = ctx;
    udp_bindings[slot].used = true;
    local_spinlock_unlock(&udp_bind_lock);
    return 0;
}

int udp_unbind(uint16_t port) {
    local_spinlock_lock(&udp_bind_lock);
    for (int i = 0; i < UDP_MAX_BINDINGS; i++) {
        if (udp_bindings[i].used && udp_bindings[i].port == port) {
            udp_bindings[i].used = false;
            local_spinlock_unlock(&udp_bind_lock);
            return 0;
        }
    }
    local_spinlock_unlock(&udp_bind_lock);
    return -1;
}

int udp_send_to(void *iface_v, uint32_t dst_ip, const uint8_t *dst_mac,
                uint16_t src_port, uint16_t dst_port,
                const void *payload, int len) {
    if (iface_v == NULL || dst_mac == NULL || len < 0)
        return -1;
    if (len > 65535 - (int)sizeof(udp_t))  // would overflow the 16-bit UDP length
        return -1;

    interface_def_t *iface = (interface_def_t *)iface_v;
    int seg_len = (int)sizeof(udp_t) + len;

    uint8_t *buf = (uint8_t *)malloc(seg_len);
    if (buf == NULL)
        return -1;

    udp_t *udp = (udp_t *)buf;
    udp->src_port = TO_BE_FRM_LE_16(src_port);
    udp->dst_port = TO_BE_FRM_LE_16(dst_port);
    udp->len = TO_BE_FRM_LE_16((uint16_t)seg_len);
    udp->csum = 0;
    if (payload != NULL && len > 0)
        memcpy(udp->body, payload, len);

    uint32_t sum = udp_ipv4_pseudo_sum(iface->ip, dst_ip, udp->len);
    uint16_t csum = net_csum_fold(net_csum_acc(sum, udp, seg_len));
    // A computed checksum of zero is transmitted as all-ones, since 0 on the wire
    // means "no checksum" for UDP-over-IPv4 (RFC 768).
    udp->csum = (csum == 0) ? 0xFFFF : csum;

    int ret = ipv4_tx(iface, dst_ip, dst_mac, IP_PROTOCOL_UDP, buf, seg_len);

    free(buf);
    return ret;
}

int udp_ipv4_rx(interface_def_t *interface, const uint8_t *src_mac, ipv4_t *packet,
                int hdr_len, int len) {
    // The UDP header begins after the IPv4 header (hdr_len, which accounts for
    // options when ihl > 5), not at a fixed offset. The caller has validated
    // that hdr_len + len lies within the received frame.
    udp_t *udp = (udp_t *)((uint8_t *)packet + hdr_len);
    int seg_len = udp_seg_len(udp, len);
    if (seg_len < 0)
        return 0;  // malformed -> drop

    // A transmitted checksum of 0 means "no checksum" for UDP-over-IPv4.
    if (udp->csum != 0 && !udp_ipv4_csum_ok(packet, udp, seg_len))
        return 0;  // bad checksum -> drop

    uint16_t dst_port = TO_LE_FRM_BE_16(udp->dst_port);
    uint16_t src_port = TO_LE_FRM_BE_16(udp->src_port);

    // Look up the bound handler under the lock, then release it before invoking
    // the handler (which may reply via udp_send_to / re-enter the table).
    udp_handler_t handler = NULL;
    void *ctx = NULL;
    local_spinlock_lock(&udp_bind_lock);
    for (int i = 0; i < UDP_MAX_BINDINGS; i++) {
        if (udp_bindings[i].used && udp_bindings[i].port == dst_port) {
            handler = udp_bindings[i].handler;
            ctx = udp_bindings[i].ctx;
            break;
        }
    }
    local_spinlock_unlock(&udp_bind_lock);

    if (handler != NULL) {
        int payload_len = seg_len - (int)sizeof(udp_t);
        handler(ctx, interface, packet->src_ip, src_mac, src_port, dst_port,
                udp->body, payload_len);
    }
    // No binding for this port -> drop silently.
    return 0;
}

static int udp_ipv6_csum_ok(const ipv6_t *packet, const udp_t *udp, int seg_len) {
    uint8_t ph[40];
    memcpy(ph + 0, packet->src_ip, 16);
    memcpy(ph + 16, packet->dst_ip, 16);
    // Upper-layer length is the sender's on-wire UDP length (RFC 8200 pseudo-
    // header), matching the IPv4 path -- not the clamped seg_len, or a truncated
    // packet's pseudo-header would diverge from what the sender summed.
    uint32_t ulen_be = TO_BE_FRM_LE_32((uint32_t)TO_LE_FRM_BE_16(udp->len));
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
