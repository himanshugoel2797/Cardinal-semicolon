// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "SysTest/test.h"

#include "CoreNetwork/driver.h"
#include "CoreNetwork/net.h"

#include "net_priv.h"
#include "arp.h"
#include "ethernet.h"
#include "ip.h"
#include "udp.h"
#include "icmp.h"
#include "checksum.h"

// The *_HEADER_LEN constants are what the protocol code uses to size and offset
// into on-wire buffers; the packed structs are how that same code actually reads
// and writes the fields. The two must agree, so compare the constants against
// the real sizeof() of the wire structs. This catches a struct-layout
// regression (e.g. lost packing, a reordered/resized field) -- comparing a
// constant against its own literal would not.
static void test_header_lengths(test_ctx_t *ctx) {
    // arp_t / ipv4_t carry no trailing payload in the header proper (ipv4_t's
    // body[] is a zero-length flexible member, excluded from sizeof).
    TEST_CHECK_EQ_U(ctx, ARP_HEADER_LEN, sizeof(arp_t));
    TEST_CHECK_EQ_U(ctx, IPV4_HEADER_LEN, sizeof(ipv4_t));
    TEST_CHECK_EQ_U(ctx, UDP_HEADER_LEN, sizeof(udp_t));

    // Concrete wire sizes, so a silently mis-packed struct is caught even if its
    // matching constant drifted with it.
    TEST_CHECK_EQ_U(ctx, sizeof(arp_t), 28);
    TEST_CHECK_EQ_U(ctx, sizeof(ipv4_t), 20);
    TEST_CHECK_EQ_U(ctx, sizeof(udp_t), 8);

    TEST_CHECK_EQ_U(ctx, UDP_IPV4_PACKET_SPACE,
                    IPV4_HEADER_LEN + UDP_HEADER_LEN);
}

// Registering an ethernet device with a trivial descriptor should succeed and
// hand back a non-NULL interface handle.
static void test_network_register(test_ctx_t *ctx) {
    network_device_desc_t desc;
    for (size_t i = 0; i < sizeof(desc); i++)
        ((uint8_t *)&desc)[i] = 0;

    desc.name[0] = '\0';
    desc.type = network_device_type_ethernet;
    desc.handlers.ether.tx = NULL;
    desc.handlers.ether.link_status = NULL;
    desc.lock = 0;

    void *handle = NULL;
    int ret = network_register(&desc, &handle);

    TEST_CHECK_EQ_U(ctx, ret, 0);
    TEST_CHECK(ctx, handle != NULL);
}

// --- Shared TX-capture plumbing for the rx-dispatch / reply tests --------------
//
// ethernet_tx() frames a payload and hands the finished frame to the
// interface's driver tx handler, then frees it. To observe what a receive path
// *transmitted* (an ARP reply, an ICMP echo reply) we register a mock device
// whose tx handler copies the outgoing frame into a static buffer before it is
// freed. This lets the rx tests assert on real on-wire output without any
// hardware.

#define MOCK_TX_BUF 256
static uint8_t mock_tx_frame[MOCK_TX_BUF];
static int mock_tx_len;
static int mock_tx_count;

static const uint8_t mock_if_mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
static const uint8_t peer_mac[6] = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};

static int mock_tx(void *state, void *packet, int len, network_device_tx_flags_t flags) {
    (void)state;
    (void)flags;
    mock_tx_count++;
    mock_tx_len = len;
    if (len > 0 && len <= MOCK_TX_BUF)
        memcpy(mock_tx_frame, packet, (size_t)len);
    return 0;
}

// Build an interface backed by mock_tx with a known MAC. The handle returned by
// network_register is the interface_def_t we drive ethernet_rx against.
static interface_def_t *make_mock_interface(test_ctx_t *ctx) {
    network_device_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.name[0] = '\0';
    desc.type = network_device_type_ethernet;
    desc.handlers.ether.tx = mock_tx;
    desc.handlers.ether.link_status = NULL;
    for (int i = 0; i < 6; i++)
        desc.mac[i] = mock_if_mac[i];
    desc.lock = 0;

    void *handle = NULL;
    int ret = network_register(&desc, &handle);
    TEST_CHECK_EQ_U(ctx, ret, 0);
    TEST_CHECK(ctx, handle != NULL);
    return (interface_def_t *)handle;
}

static void reset_tx_capture(void) {
    mock_tx_count = 0;
    mock_tx_len = 0;
    memset(mock_tx_frame, 0, sizeof(mock_tx_frame));
}

// The ARP cache is a plain in-memory map: an update followed by a lookup of the
// same (network-order) IP must return the stored MAC; an unknown IP must miss.
// Pure logic, safe inline.
static void test_arp_cache_roundtrip(test_ctx_t *ctx) {
    uint32_t ip = IPV4_ADDR(192, 168, 7, 42);
    const uint8_t mac[6] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xab};

    arp_cache_update(ip, mac);

    uint8_t out[6];
    memset(out, 0, sizeof(out));
    bool hit = arp_cache_lookup(ip, out);
    TEST_CHECK(ctx, hit);
    TEST_CHECK_MSG(ctx, memcmp(out, mac, 6) == 0,
                   "arp_cache_lookup returned a MAC that does not match the stored one");

    // An address never inserted must miss (and must not clobber out on a miss).
    uint8_t miss_out[6];
    memset(miss_out, 0xCC, sizeof(miss_out));
    bool miss = arp_cache_lookup(IPV4_ADDR(203, 0, 113, 1), miss_out);
    TEST_CHECK(ctx, !miss);

    // Re-inserting the same IP with a new MAC must overwrite, not duplicate.
    const uint8_t mac2[6] = {0xde, 0xad, 0xbe, 0xef, 0x00, 0x11};
    arp_cache_update(ip, mac2);
    memset(out, 0, sizeof(out));
    hit = arp_cache_lookup(ip, out);
    TEST_CHECK(ctx, hit);
    TEST_CHECK_MSG(ctx, memcmp(out, mac2, 6) == 0,
                   "arp_cache_update did not refresh an existing entry's MAC");
}

// ethernet_rx must dispatch an ARP request addressed to our interface IP up to
// the ARP layer, which (a) learns the sender in the cache and (b) transmits an
// ARP reply via the driver tx path. This exercises ethernet_rx's
// ethertype-demux *and* arp_rx's request/reply construction end-to-end.
static void test_ethernet_rx_arp_reply(test_ctx_t *ctx) {
    interface_def_t *iface = make_mock_interface(ctx);
    if (iface == NULL) {
        TEST_FAIL(ctx, "interface registration failed");
        return;
    }
    reset_tx_capture();

    uint32_t sender_ip = IPV4_ADDR(10, 0, 2, 99);

    // Ethernet header + ARP request: "who has <iface->ip>? tell sender".
    uint8_t frame[sizeof(ethernet_frame_t) + sizeof(arp_t)];
    memset(frame, 0, sizeof(frame));
    ethernet_frame_t *eth = (ethernet_frame_t *)frame;
    for (int i = 0; i < 6; i++) {
        eth->dst_mac[i] = mock_if_mac[i];  // unicast to us
        eth->src_mac[i] = peer_mac[i];
    }
    eth->type = ETHERNET_TYPE_ARP;

    arp_t *arp = (arp_t *)eth->body;
    arp->hw_type = TO_BE_FRM_LE_16(ARP_HW_TYPE_ETHERNET);
    arp->protocol_type = TO_BE_FRM_LE_16(ARP_PROTO_TYPE_IPV4);
    arp->hw_addr_len = 6;
    arp->protocol_addr_len = 4;
    arp->opcode = TO_BE_FRM_LE_16(ARP_REQUEST);
    for (int i = 0; i < 6; i++)
        arp->src_mac[i] = peer_mac[i];
    arp->src_ip = sender_ip;
    memset(arp->dst_mac, 0, 6);
    arp->dst_ip = iface->ip;  // targets our interface -> reply expected

    ethernet_rx(iface, frame, (int)sizeof(frame));

    // The sender should now be in the ARP cache (learned regardless of opcode).
    uint8_t learned[6];
    memset(learned, 0, sizeof(learned));
    TEST_CHECK_MSG(ctx, arp_cache_lookup(sender_ip, learned),
                   "ARP request sender was not learned into the cache");
    TEST_CHECK(ctx, memcmp(learned, peer_mac, 6) == 0);

    // Exactly one reply frame should have been transmitted.
    TEST_CHECK_EQ_U(ctx, mock_tx_count, 1);
    TEST_CHECK(ctx, mock_tx_len >= (int)(sizeof(ethernet_frame_t) + sizeof(arp_t)));
    if (mock_tx_len >= (int)(sizeof(ethernet_frame_t) + sizeof(arp_t))) {
        ethernet_frame_t *rep_eth = (ethernet_frame_t *)mock_tx_frame;
        // Reply is an ARP frame, sourced from our MAC, destined to the requester.
        TEST_CHECK_EQ_U(ctx, rep_eth->type, ETHERNET_TYPE_ARP);
        TEST_CHECK(ctx, memcmp(rep_eth->src_mac, mock_if_mac, 6) == 0);
        TEST_CHECK(ctx, memcmp(rep_eth->dst_mac, peer_mac, 6) == 0);

        arp_t *rep = (arp_t *)rep_eth->body;
        TEST_CHECK_EQ_U(ctx, TO_LE_FRM_BE_16(rep->opcode), ARP_REPLY);
        // src/dst IPs are swapped relative to the request.
        TEST_CHECK_EQ_U(ctx, rep->src_ip, iface->ip);
        TEST_CHECK_EQ_U(ctx, rep->dst_ip, sender_ip);
        TEST_CHECK(ctx, memcmp(rep->src_mac, mock_if_mac, 6) == 0);
    }
}

// An ICMP echo request addressed to our interface, delivered through
// ethernet_rx -> ipv4_rx -> icmp_ipv4_rx, must produce an echo *reply*: same
// id/seq/data, type flipped to ECHO_REPLY, with valid IPv4 and ICMP checksums.
static void test_ethernet_rx_icmp_echo_reply(test_ctx_t *ctx) {
    interface_def_t *iface = make_mock_interface(ctx);
    if (iface == NULL) {
        TEST_FAIL(ctx, "interface registration failed");
        return;
    }
    reset_tx_capture();

    uint32_t sender_ip = IPV4_ADDR(10, 0, 2, 100);

    // ICMP echo payload: 4 bytes id/seq + 8 bytes data.
    const int icmp_data = 8;
    const int icmp_len = (int)sizeof(icmp_t) + 4 + icmp_data;
    const int ip_total = (int)sizeof(ipv4_t) + icmp_len;
    const int frame_len = (int)sizeof(ethernet_frame_t) + ip_total;

    uint8_t frame[sizeof(ethernet_frame_t) + sizeof(ipv4_t) + sizeof(icmp_t) + 4 + 8];
    memset(frame, 0, sizeof(frame));

    ethernet_frame_t *eth = (ethernet_frame_t *)frame;
    for (int i = 0; i < 6; i++) {
        eth->dst_mac[i] = mock_if_mac[i];
        eth->src_mac[i] = peer_mac[i];
    }
    eth->type = ETHERNET_TYPE_IPv4;

    ipv4_t *ip = (ipv4_t *)eth->body;
    ip->version = 4;
    ip->ihl = 5;
    ip->total_len = TO_BE_FRM_LE_16((uint16_t)ip_total);
    ip->ttl = 64;
    ip->protocol = IP_PROTOCOL_ICMP;
    ip->src_ip = sender_ip;
    ip->dst_ip = iface->ip;  // addressed to us
    ip->hdr_csum = 0;
    ip->hdr_csum = net_checksum16(ip, (int)sizeof(ipv4_t));

    icmp_t *icmp = (icmp_t *)ip->body;
    icmp->type = ICMP_TYPE_ECHO_REQUEST;
    icmp->code = 0;
    // id/seq + data, a recognizable pattern to confirm the body is echoed back.
    uint8_t *payload = icmp->body;
    for (int i = 0; i < 4 + icmp_data; i++)
        payload[i] = (uint8_t)(0x20 + i);
    icmp->csum = 0;
    icmp->csum = net_checksum16(icmp, icmp_len);

    ethernet_rx(iface, frame, frame_len);

    // One echo reply should have been transmitted.
    TEST_CHECK_EQ_U(ctx, mock_tx_count, 1);
    int min = (int)(sizeof(ethernet_frame_t) + sizeof(ipv4_t) + sizeof(icmp_t));
    TEST_CHECK(ctx, mock_tx_len >= min);
    if (mock_tx_len < min)
        return;

    ethernet_frame_t *r_eth = (ethernet_frame_t *)mock_tx_frame;
    TEST_CHECK_EQ_U(ctx, r_eth->type, ETHERNET_TYPE_IPv4);

    ipv4_t *r_ip = (ipv4_t *)r_eth->body;
    TEST_CHECK_EQ_U(ctx, r_ip->version, 4);
    TEST_CHECK_EQ_U(ctx, r_ip->protocol, IP_PROTOCOL_ICMP);
    // Reply is sourced from us, destined to the original sender.
    TEST_CHECK_EQ_U(ctx, r_ip->src_ip, iface->ip);
    TEST_CHECK_EQ_U(ctx, r_ip->dst_ip, sender_ip);
    // IPv4 header checksum must validate (fold to zero over the header).
    TEST_CHECK_MSG(ctx, net_checksum16(r_ip, (int)sizeof(ipv4_t)) == 0,
                   "reply IPv4 header checksum does not validate");

    icmp_t *r_icmp = (icmp_t *)r_ip->body;
    TEST_CHECK_EQ_U(ctx, r_icmp->type, ICMP_TYPE_ECHO_REPLY);
    TEST_CHECK_EQ_U(ctx, r_icmp->code, 0);
    // ICMP message checksum must validate.
    TEST_CHECK_MSG(ctx, net_checksum16(r_icmp, icmp_len) == 0,
                   "reply ICMP checksum does not validate");
    // Body (id/seq/data) must be echoed back byte-for-byte.
    TEST_CHECK_MSG(ctx, memcmp(r_icmp->body, payload, 4 + icmp_data) == 0,
                   "ICMP echo reply did not echo the request body");
}

// A malformed UDP datagram (its on-wire length field claims fewer bytes than the
// UDP header) must be dropped by udp_ipv4_rx without crashing and without
// transmitting anything. udp_ipv4_rx is a leaf (the success path is a deferred
// TODO that emits nothing), so the observable contract is: returns 0, no TX.
static void test_udp_malformed_drop(test_ctx_t *ctx) {
    interface_def_t *iface = make_mock_interface(ctx);
    if (iface == NULL) {
        TEST_FAIL(ctx, "interface registration failed");
        return;
    }
    reset_tx_capture();

    // Construct an IPv4+UDP buffer where udp->len is bogus (1 < sizeof(udp_t)).
    uint8_t buf[sizeof(ipv4_t) + sizeof(udp_t)];
    memset(buf, 0, sizeof(buf));
    ipv4_t *ip = (ipv4_t *)buf;
    ip->version = 4;
    ip->ihl = 5;
    ip->protocol = IP_PROTOCOL_UDP;
    ip->src_ip = IPV4_ADDR(10, 0, 2, 50);
    ip->dst_ip = iface->ip;

    udp_t *udp = (udp_t *)ip->body;
    udp->src_port = TO_BE_FRM_LE_16(1234);
    udp->dst_port = TO_BE_FRM_LE_16(5678);
    udp->len = TO_BE_FRM_LE_16(1);  // malformed: shorter than the UDP header
    udp->csum = 0;

    int rc = udp_ipv4_rx(iface, ip, (int)sizeof(ipv4_t), (int)sizeof(udp_t));
    TEST_CHECK_EQ_U(ctx, rc, 0);
    TEST_CHECK_MSG(ctx, mock_tx_count == 0,
                   "malformed UDP datagram triggered a transmission");

    // Also: a truncated segment (fewer bytes than the UDP header) is dropped.
    rc = udp_ipv4_rx(iface, ip, (int)sizeof(ipv4_t), (int)sizeof(udp_t) - 1);
    TEST_CHECK_EQ_U(ctx, rc, 0);
    TEST_CHECK_EQ_U(ctx, mock_tx_count, 0);
}

void corenetwork_register_tests(void) {
    if (!test_mode_active())
        return;

    {
        test_def_t t = {
            .suite = "CoreNetwork",
            .name = "header_lengths",
            .fn = test_header_lengths,
            .run = TEST_RUN_INLINE,
            .flags = TEST_FLAG_NONE,
        };
        test_register(&t);
    }

    {
        test_def_t t = {
            .suite = "CoreNetwork",
            .name = "network_register",
            .fn = test_network_register,
            .run = TEST_RUN_INLINE,
            .flags = TEST_FLAG_NONE,
        };
        test_register(&t);
    }

    {
        test_def_t t = {
            .suite = "CoreNetwork",
            .name = "arp_cache_roundtrip",
            .fn = test_arp_cache_roundtrip,
            .run = TEST_RUN_INLINE,
            .flags = TEST_FLAG_NONE,
        };
        test_register(&t);
    }

    {
        test_def_t t = {
            .suite = "CoreNetwork",
            .name = "ethernet_rx_arp_reply",
            .fn = test_ethernet_rx_arp_reply,
            .run = TEST_RUN_INLINE,
            .flags = TEST_FLAG_NONE,
        };
        test_register(&t);
    }

    {
        test_def_t t = {
            .suite = "CoreNetwork",
            .name = "ethernet_rx_icmp_echo_reply",
            .fn = test_ethernet_rx_icmp_echo_reply,
            .run = TEST_RUN_INLINE,
            .flags = TEST_FLAG_NONE,
        };
        test_register(&t);
    }

    {
        test_def_t t = {
            .suite = "CoreNetwork",
            .name = "udp_malformed_drop",
            .fn = test_udp_malformed_drop,
            .run = TEST_RUN_INLINE,
            .flags = TEST_FLAG_NONE,
        };
        test_register(&t);
    }
}
