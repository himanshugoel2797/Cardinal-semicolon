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
#include "CoreNetwork/udp.h"
#include "CoreNetwork/rdt.h"

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

    int rc = udp_ipv4_rx(iface, peer_mac, ip, (int)sizeof(ipv4_t), (int)sizeof(udp_t));
    TEST_CHECK_EQ_U(ctx, rc, 0);
    TEST_CHECK_MSG(ctx, mock_tx_count == 0,
                   "malformed UDP datagram triggered a transmission");

    // Also: a truncated segment (fewer bytes than the UDP header) is dropped.
    rc = udp_ipv4_rx(iface, peer_mac, ip, (int)sizeof(ipv4_t), (int)sizeof(udp_t) - 1);
    TEST_CHECK_EQ_U(ctx, rc, 0);
    TEST_CHECK_EQ_U(ctx, mock_tx_count, 0);
}

// --- UDP send / port-dispatch -------------------------------------------------

// Compute the UDP-over-IPv4 checksum (pseudo-header + segment) the value of
// which, stored in udp->csum, makes the segment fold to zero. Leaves udp->csum
// as it found it.
static uint16_t udp_calc_csum(ipv4_t *ip, udp_t *udp, int seg) {
    uint16_t save = udp->csum;
    udp->csum = 0;
    uint8_t ph[12];
    memcpy(ph + 0, &ip->src_ip, 4);
    memcpy(ph + 4, &ip->dst_ip, 4);
    ph[8] = 0;
    ph[9] = IP_PROTOCOL_UDP;
    memcpy(ph + 10, &udp->len, 2);
    uint32_t s = net_csum_acc(0, ph, sizeof(ph));
    s = net_csum_acc(s, udp, seg);
    udp->csum = save;
    return net_csum_fold(s);
}

// Build an IPv4+UDP datagram in `backing` (csum=0 -> "no checksum", accepted) and
// feed it through the real udp_ipv4_rx demux. `backing` must hold
// sizeof(ipv4_t)+sizeof(udp_t)+plen bytes. dst_ip is the interface IP.
static void feed_udp(interface_def_t *iface, uint32_t src_ip, uint16_t src_port,
                     uint16_t dst_port, const void *payload, int plen,
                     uint8_t *backing) {
    ipv4_t *ip = (ipv4_t *)backing;
    memset(ip, 0, sizeof(ipv4_t));
    ip->version = 4;
    ip->ihl = 5;
    ip->protocol = IP_PROTOCOL_UDP;
    ip->src_ip = src_ip;
    ip->dst_ip = iface->ip;
    int seg = (int)sizeof(udp_t) + plen;
    ip->total_len = TO_BE_FRM_LE_16((uint16_t)((int)sizeof(ipv4_t) + seg));

    udp_t *udp = (udp_t *)ip->body;
    udp->src_port = TO_BE_FRM_LE_16(src_port);
    udp->dst_port = TO_BE_FRM_LE_16(dst_port);
    udp->len = TO_BE_FRM_LE_16((uint16_t)seg);
    udp->csum = 0;
    if (plen > 0)
        memcpy(udp->body, payload, plen);

    udp_ipv4_rx(iface, peer_mac, ip, (int)sizeof(ipv4_t), seg);
}

static int g_udp_rx_count;
static uint32_t g_udp_rx_src_ip;
static uint16_t g_udp_rx_src_port, g_udp_rx_dst_port;
static int g_udp_rx_len;
static uint8_t g_udp_rx_buf[64];

static void udp_test_handler(void *c, void *iface, uint32_t src_ip,
                             const uint8_t *src_mac, uint16_t sp, uint16_t dp,
                             const void *pl, int len) {
    (void)c;
    (void)iface;
    (void)src_mac;
    g_udp_rx_count++;
    g_udp_rx_src_ip = src_ip;
    g_udp_rx_src_port = sp;
    g_udp_rx_dst_port = dp;
    g_udp_rx_len = len;
    if (len > 0 && len <= (int)sizeof(g_udp_rx_buf))
        memcpy(g_udp_rx_buf, pl, len);
}

// udp_send_to must emit a single, well-formed IPv4+UDP frame with valid IPv4 and
// UDP (pseudo-header) checksums and the requested ports/payload.
static void test_udp_send_to(test_ctx_t *ctx) {
    interface_def_t *iface = make_mock_interface(ctx);
    if (iface == NULL) {
        TEST_FAIL(ctx, "interface registration failed");
        return;
    }
    reset_tx_capture();

    uint32_t dst_ip = IPV4_ADDR(10, 0, 2, 2);
    const char *msg = "hello-udp";
    int mlen = (int)strlen(msg);

    int rc = udp_send_to(iface, dst_ip, peer_mac, 4444, 9999, msg, mlen);
    TEST_CHECK_EQ_U(ctx, rc, 0);
    TEST_CHECK_EQ_U(ctx, mock_tx_count, 1);

    int min = (int)(sizeof(ethernet_frame_t) + sizeof(ipv4_t) + sizeof(udp_t)) + mlen;
    TEST_CHECK(ctx, mock_tx_len >= min);
    if (mock_tx_len < min)
        return;

    ethernet_frame_t *eth = (ethernet_frame_t *)mock_tx_frame;
    TEST_CHECK_EQ_U(ctx, eth->type, ETHERNET_TYPE_IPv4);
    TEST_CHECK(ctx, memcmp(eth->dst_mac, peer_mac, 6) == 0);

    ipv4_t *ip = (ipv4_t *)eth->body;
    TEST_CHECK_EQ_U(ctx, ip->protocol, IP_PROTOCOL_UDP);
    TEST_CHECK_EQ_U(ctx, ip->dst_ip, dst_ip);
    TEST_CHECK_EQ_U(ctx, ip->src_ip, iface->ip);
    TEST_CHECK_MSG(ctx, net_checksum16(ip, (int)sizeof(ipv4_t)) == 0,
                   "udp_send_to produced a bad IPv4 header checksum");

    udp_t *udp = (udp_t *)ip->body;
    TEST_CHECK_EQ_U(ctx, TO_LE_FRM_BE_16(udp->src_port), 4444);
    TEST_CHECK_EQ_U(ctx, TO_LE_FRM_BE_16(udp->dst_port), 9999);
    TEST_CHECK_EQ_U(ctx, TO_LE_FRM_BE_16(udp->len), (uint16_t)((int)sizeof(udp_t) + mlen));
    TEST_CHECK(ctx, memcmp(udp->body, msg, mlen) == 0);
    // UDP checksum (with pseudo-header) must validate -> the segment folds to 0.
    TEST_CHECK_MSG(ctx, udp_calc_csum(ip, udp, (int)sizeof(udp_t) + mlen) == 0 ||
                            udp->csum != 0,
                   "udp_send_to left a zero (absent) checksum");
    {
        uint8_t ph[12];
        memcpy(ph + 0, &ip->src_ip, 4);
        memcpy(ph + 4, &ip->dst_ip, 4);
        ph[8] = 0;
        ph[9] = IP_PROTOCOL_UDP;
        memcpy(ph + 10, &udp->len, 2);
        uint32_t s = net_csum_acc(0, ph, sizeof(ph));
        s = net_csum_acc(s, udp, (int)sizeof(udp_t) + mlen);
        TEST_CHECK_MSG(ctx, net_csum_fold(s) == 0,
                       "udp_send_to produced a bad UDP checksum");
    }
}

// A bound port receives its datagrams (with the sender's ip/ports/payload); a
// bad UDP checksum is dropped; an unbound port is dropped.
static void test_udp_rx_dispatch(test_ctx_t *ctx) {
    interface_def_t *iface = make_mock_interface(ctx);
    if (iface == NULL) {
        TEST_FAIL(ctx, "interface registration failed");
        return;
    }
    const uint16_t port = 9001;
    TEST_CHECK_EQ_U(ctx, udp_bind(port, udp_test_handler, NULL), 0);
    // Binding the same port twice must fail.
    TEST_CHECK(ctx, udp_bind(port, udp_test_handler, NULL) != 0);

    uint32_t peer_ip = IPV4_ADDR(10, 0, 2, 77);
    const char *msg = "ping-payload";
    int mlen = (int)strlen(msg);
    uint8_t backing[64];

    g_udp_rx_count = 0;
    feed_udp(iface, peer_ip, 5555, port, msg, mlen, backing);
    TEST_CHECK_EQ_U(ctx, g_udp_rx_count, 1);
    TEST_CHECK_EQ_U(ctx, g_udp_rx_src_ip, peer_ip);
    TEST_CHECK_EQ_U(ctx, g_udp_rx_src_port, 5555);
    TEST_CHECK_EQ_U(ctx, g_udp_rx_dst_port, port);
    TEST_CHECK_EQ_U(ctx, g_udp_rx_len, mlen);
    TEST_CHECK(ctx, memcmp(g_udp_rx_buf, msg, mlen) == 0);

    // Unbound port -> dropped (handler not called).
    g_udp_rx_count = 0;
    feed_udp(iface, peer_ip, 5555, 9002, msg, mlen, backing);
    TEST_CHECK_EQ_U(ctx, g_udp_rx_count, 0);

    // Non-zero but wrong checksum -> dropped.
    {
        ipv4_t *ip = (ipv4_t *)backing;
        memset(ip, 0, sizeof(ipv4_t));
        ip->version = 4;
        ip->ihl = 5;
        ip->protocol = IP_PROTOCOL_UDP;
        ip->src_ip = peer_ip;
        ip->dst_ip = iface->ip;
        int seg = (int)sizeof(udp_t) + mlen;
        ip->total_len = TO_BE_FRM_LE_16((uint16_t)((int)sizeof(ipv4_t) + seg));
        udp_t *udp = (udp_t *)ip->body;
        udp->src_port = TO_BE_FRM_LE_16(5555);
        udp->dst_port = TO_BE_FRM_LE_16(port);
        udp->len = TO_BE_FRM_LE_16((uint16_t)seg);
        udp->csum = 0;
        memcpy(udp->body, msg, mlen);
        uint16_t good = udp_calc_csum(ip, udp, seg);
        // A value that is neither correct nor zero ("absent") -> must reject.
        udp->csum = (good == 1) ? 2 : (uint16_t)(good ^ 1);

        g_udp_rx_count = 0;
        udp_ipv4_rx(iface, peer_mac, ip, (int)sizeof(ipv4_t), seg);
        TEST_CHECK_MSG(ctx, g_udp_rx_count == 0,
                       "UDP datagram with a bad checksum was delivered");
    }

    udp_unbind(port);
}

// --- RDT reliable transport ---------------------------------------------------

// Build an RDT datagram (header + trailing) into `out`, with a valid checksum,
// matching servers/inc/CoreNetwork/rdt.h. Returns the total length.
static int build_rdt(uint8_t *out, uint8_t type, uint8_t flags, uint16_t name_len,
                     uint32_t xfer_id, uint32_t total_len, uint32_t offset,
                     uint16_t chunk_len, const void *trailing, int tlen) {
    rdt_hdr_t *h = (rdt_hdr_t *)out;
    memset(h, 0, sizeof(*h));
    h->magic[0] = RDT_MAGIC0;
    h->magic[1] = RDT_MAGIC1;
    h->magic[2] = RDT_MAGIC2;
    h->magic[3] = RDT_MAGIC3;
    h->type = type;
    h->flags = flags;
    h->name_len = TO_BE_FRM_LE_16(name_len);
    h->xfer_id = TO_BE_FRM_LE_32(xfer_id);
    h->total_len = TO_BE_FRM_LE_32(total_len);
    h->offset = TO_BE_FRM_LE_32(offset);
    h->chunk_len = TO_BE_FRM_LE_16(chunk_len);
    h->csum = 0;
    if (trailing != NULL && tlen > 0)
        memcpy(out + sizeof(rdt_hdr_t), trailing, tlen);

    uint32_t s = net_csum_acc(0, h, (int)sizeof(rdt_hdr_t) - 2);
    if (trailing != NULL && tlen > 0)
        s = net_csum_acc(s, out + sizeof(rdt_hdr_t), tlen);
    h->csum = TO_BE_FRM_LE_16(net_csum_fold(s));
    return (int)sizeof(rdt_hdr_t) + tlen;
}

// The most recent ACK the device transmitted (parsed out of the captured frame).
static rdt_hdr_t *captured_ack(test_ctx_t *ctx) {
    int off = (int)(sizeof(ethernet_frame_t) + sizeof(ipv4_t) + sizeof(udp_t));
    if (mock_tx_len < off + (int)sizeof(rdt_hdr_t)) {
        TEST_FAIL(ctx, "captured frame too short to hold an RDT ACK");
        return NULL;
    }
    return (rdt_hdr_t *)(mock_tx_frame + off);
}

static int g_rdt_count;
static int g_rdt_len;
static uint8_t g_rdt_buf[64];
static char g_rdt_name[RDT_MAX_NAME];

static void rdt_test_sink(void *c, void *iface, uint32_t src_ip,
                          const uint8_t *src_mac, uint16_t sp, const char *name,
                          void *data, int len) {
    (void)c;
    (void)iface;
    (void)src_ip;
    (void)src_mac;
    (void)sp;
    g_rdt_count++;
    g_rdt_len = len;
    int i = 0;
    for (; i < (int)sizeof(g_rdt_name) - 1 && name[i] != '\0'; i++)
        g_rdt_name[i] = name[i];
    g_rdt_name[i] = '\0';
    if (len > 0 && len <= (int)sizeof(g_rdt_buf))
        memcpy(g_rdt_buf, data, len);
}

// A full multi-chunk transfer: START handshake, in-order DATA appends with
// cumulative ACKs, an out-of-order DATA that is re-ACKed (not applied), the
// completing DATA that fires the sink with the reassembled blob, and a
// retransmitted final DATA that is idempotently re-ACKed without re-delivering.
static void test_rdt_transfer(test_ctx_t *ctx) {
    interface_def_t *iface = make_mock_interface(ctx);
    if (iface == NULL) {
        TEST_FAIL(ctx, "interface registration failed");
        return;
    }
    TEST_CHECK_EQ_U(ctx, rdt_listen(9100, rdt_test_sink, NULL), 0);

    uint32_t peer_ip = IPV4_ADDR(10, 0, 2, 88);
    uint32_t xfer = 0xCAFE1234u;
    uint8_t data[10];
    for (int i = 0; i < 10; i++)
        data[i] = (uint8_t)(0xA0 + i);

    uint8_t pkt[64];
    uint8_t backing[128];
    int plen;
    rdt_hdr_t *ack;

    g_rdt_count = 0;
    g_rdt_len = -1;

    // START: name "blob", total 10 -> ACK offset 0, no FIN.
    plen = build_rdt(pkt, RDT_TYPE_START, 0, 4, xfer, 10, 0, 0, "blob", 4);
    reset_tx_capture();
    feed_udp(iface, peer_ip, 5000, 9100, pkt, plen, backing);
    TEST_CHECK_EQ_U(ctx, mock_tx_count, 1);
    ack = captured_ack(ctx);
    if (ack == NULL)
        return;
    TEST_CHECK_EQ_U(ctx, ack->type, RDT_TYPE_ACK);
    TEST_CHECK_EQ_U(ctx, TO_LE_FRM_BE_32(ack->offset), 0);
    TEST_CHECK(ctx, !(ack->flags & RDT_FLAG_FIN));
    TEST_CHECK_EQ_U(ctx, g_rdt_count, 0);

    // DATA chunk #0: offset 0, len 4 -> ACK offset 4, no completion.
    plen = build_rdt(pkt, RDT_TYPE_DATA, 0, 0, xfer, 10, 0, 4, data, 4);
    reset_tx_capture();
    feed_udp(iface, peer_ip, 5000, 9100, pkt, plen, backing);
    ack = captured_ack(ctx);
    if (ack == NULL)
        return;
    TEST_CHECK_EQ_U(ctx, TO_LE_FRM_BE_32(ack->offset), 4);
    TEST_CHECK(ctx, !(ack->flags & RDT_FLAG_FIN));
    TEST_CHECK_EQ_U(ctx, g_rdt_count, 0);

    // Out-of-order DATA (offset 8 while we expect 4) -> re-ACK 4, nothing applied.
    plen = build_rdt(pkt, RDT_TYPE_DATA, 0, 0, xfer, 10, 8, 2, data + 8, 2);
    reset_tx_capture();
    feed_udp(iface, peer_ip, 5000, 9100, pkt, plen, backing);
    ack = captured_ack(ctx);
    if (ack == NULL)
        return;
    TEST_CHECK_EQ_U(ctx, TO_LE_FRM_BE_32(ack->offset), 4);
    TEST_CHECK_EQ_U(ctx, g_rdt_count, 0);

    // DATA chunk #1: offset 4, len 6 -> completes; ACK offset 10 + FIN; sink fires.
    plen = build_rdt(pkt, RDT_TYPE_DATA, 0, 0, xfer, 10, 4, 6, data + 4, 6);
    reset_tx_capture();
    feed_udp(iface, peer_ip, 5000, 9100, pkt, plen, backing);
    ack = captured_ack(ctx);
    if (ack == NULL)
        return;
    TEST_CHECK_EQ_U(ctx, TO_LE_FRM_BE_32(ack->offset), 10);
    TEST_CHECK(ctx, (ack->flags & RDT_FLAG_FIN) != 0);
    TEST_CHECK_EQ_U(ctx, g_rdt_count, 1);
    TEST_CHECK_EQ_U(ctx, g_rdt_len, 10);
    TEST_CHECK_MSG(ctx, memcmp(g_rdt_buf, data, 10) == 0,
                   "RDT reassembled blob does not match what was sent");
    TEST_CHECK_MSG(ctx, strcmp(g_rdt_name, "blob") == 0,
                   "RDT delivered the wrong blob name");

    // Retransmitted final DATA -> idempotent re-ACK with FIN, no second delivery.
    reset_tx_capture();
    feed_udp(iface, peer_ip, 5000, 9100, pkt, plen, backing);
    ack = captured_ack(ctx);
    if (ack == NULL)
        return;
    TEST_CHECK_EQ_U(ctx, TO_LE_FRM_BE_32(ack->offset), 10);
    TEST_CHECK(ctx, (ack->flags & RDT_FLAG_FIN) != 0);
    TEST_CHECK_MSG(ctx, g_rdt_count == 1,
                   "retransmitted final DATA re-delivered the blob");
}

// Malformed / hostile RDT datagrams must be dropped: an oversized START, a bad
// magic, and a corrupted checksum all produce no ACK and no delivery.
static void test_rdt_reject(test_ctx_t *ctx) {
    interface_def_t *iface = make_mock_interface(ctx);
    if (iface == NULL) {
        TEST_FAIL(ctx, "interface registration failed");
        return;
    }
    // Ensure port 9100 is bound to our sink whether or not test_rdt_transfer ran
    // first: a fresh listen succeeds, a repeat returns -1 but leaves the existing
    // (identical) binding in place. Either way 9100 is served.
    rdt_listen(9100, rdt_test_sink, NULL);

    uint32_t peer_ip = IPV4_ADDR(10, 0, 2, 89);
    uint32_t xfer = 0x0BADBEEFu;
    uint8_t pkt[64];
    uint8_t backing[128];
    int plen;

    g_rdt_count = 0;

    // Oversized START (total_len > RDT_MAX_BLOB) -> dropped, no ACK.
    plen = build_rdt(pkt, RDT_TYPE_START, 0, 4, xfer, RDT_MAX_BLOB + 1u, 0, 0, "blob", 4);
    reset_tx_capture();
    feed_udp(iface, peer_ip, 6000, 9100, pkt, plen, backing);
    TEST_CHECK_MSG(ctx, mock_tx_count == 0, "oversized START was not rejected");

    // Bad magic -> dropped.
    plen = build_rdt(pkt, RDT_TYPE_START, 0, 4, xfer, 10, 0, 0, "blob", 4);
    ((rdt_hdr_t *)pkt)->magic[0] = 'X';
    reset_tx_capture();
    feed_udp(iface, peer_ip, 6000, 9100, pkt, plen, backing);
    TEST_CHECK_MSG(ctx, mock_tx_count == 0, "bad-magic RDT datagram was not rejected");

    // Corrupted checksum (flip a header byte after the csum was computed) -> dropped.
    plen = build_rdt(pkt, RDT_TYPE_START, 0, 4, xfer, 10, 0, 0, "blob", 4);
    pkt[5] ^= 0xFF;  // perturb the flags byte; stored csum no longer matches
    reset_tx_capture();
    feed_udp(iface, peer_ip, 6000, 9100, pkt, plen, backing);
    TEST_CHECK_MSG(ctx, mock_tx_count == 0, "bad-checksum RDT datagram was not rejected");

    TEST_CHECK_MSG(ctx, g_rdt_count == 0, "a rejected RDT datagram still delivered a blob");
}

// --- Outbound ARP (request build / resolve / aging) ---------------------------

// arp_send_request must broadcast a well-formed ARP request: L2 broadcast dst,
// our src MAC/IP, opcode REQUEST, target IP filled and target MAC zeroed.
static void test_arp_request_build(test_ctx_t *ctx) {
    interface_def_t *iface = make_mock_interface(ctx);
    if (iface == NULL) {
        TEST_FAIL(ctx, "interface registration failed");
        return;
    }
    reset_tx_capture();

    uint32_t target = IPV4_ADDR(10, 0, 2, 123);
    arp_send_request(iface, target);

    TEST_CHECK_EQ_U(ctx, mock_tx_count, 1);
    int min = (int)(sizeof(ethernet_frame_t) + sizeof(arp_t));
    TEST_CHECK(ctx, mock_tx_len >= min);
    if (mock_tx_len < min)
        return;

    const uint8_t bcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    const uint8_t zero[6] = {0, 0, 0, 0, 0, 0};
    ethernet_frame_t *eth = (ethernet_frame_t *)mock_tx_frame;
    TEST_CHECK_EQ_U(ctx, eth->type, ETHERNET_TYPE_ARP);
    TEST_CHECK(ctx, memcmp(eth->dst_mac, bcast, 6) == 0);
    TEST_CHECK(ctx, memcmp(eth->src_mac, mock_if_mac, 6) == 0);

    arp_t *a = (arp_t *)eth->body;
    TEST_CHECK_EQ_U(ctx, TO_LE_FRM_BE_16(a->opcode), ARP_REQUEST);
    TEST_CHECK_EQ_U(ctx, a->src_ip, iface->ip);
    TEST_CHECK_EQ_U(ctx, a->dst_ip, target);
    TEST_CHECK(ctx, memcmp(a->src_mac, mock_if_mac, 6) == 0);
    TEST_CHECK(ctx, memcmp(a->dst_mac, zero, 6) == 0);
}

// A fresh cache entry resolves immediately, with no request transmitted.
static void test_arp_resolve_cached(test_ctx_t *ctx) {
    interface_def_t *iface = make_mock_interface(ctx);
    if (iface == NULL) {
        TEST_FAIL(ctx, "interface registration failed");
        return;
    }
    uint32_t ip = IPV4_ADDR(10, 0, 2, 201);
    const uint8_t mac[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
    arp_cache_update(ip, mac);

    reset_tx_capture();
    uint8_t out[6];
    memset(out, 0, sizeof(out));
    TEST_CHECK_EQ_U(ctx, arp_resolve(iface, ip, out), 0);
    TEST_CHECK(ctx, memcmp(out, mac, 6) == 0);
    TEST_CHECK_MSG(ctx, mock_tx_count == 0,
                   "arp_resolve broadcast a request for a cached entry");
}

// The pure expiry predicate: 0 == never; expired only when now has reached it.
static void test_arp_expiry_logic(test_ctx_t *ctx) {
    TEST_CHECK(ctx, !arp_entry_expired(0, 0));
    TEST_CHECK(ctx, !arp_entry_expired(0, 1000000000ull));
    TEST_CHECK(ctx, !arp_entry_expired(1000, 999));
    TEST_CHECK(ctx, arp_entry_expired(1000, 1000));
    TEST_CHECK(ctx, arp_entry_expired(1000, 5000));
}

// Resolving an address nobody answers must give up (bounded) and report -1,
// having broadcast at least one request. Sleeps -> runs as a task.
static void test_arp_resolve_timeout(test_ctx_t *ctx) {
    interface_def_t *iface = make_mock_interface(ctx);
    if (iface == NULL) {
        TEST_FAIL(ctx, "interface registration failed");
        return;
    }
    reset_tx_capture();
    uint8_t out[6];
    uint32_t ip = IPV4_ADDR(10, 0, 2, 222);  // no peer will reply in the test env
    TEST_CHECK_EQ_U(ctx, arp_resolve(iface, ip, out), -1);
    TEST_CHECK_MSG(ctx, mock_tx_count >= 1,
                   "arp_resolve never broadcast a request on a cache miss");
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

    {
        test_def_t t = {
            .suite = "CoreNetwork",
            .name = "udp_send_to",
            .fn = test_udp_send_to,
            .run = TEST_RUN_INLINE,
            .flags = TEST_FLAG_NONE,
        };
        test_register(&t);
    }

    {
        test_def_t t = {
            .suite = "CoreNetwork",
            .name = "udp_rx_dispatch",
            .fn = test_udp_rx_dispatch,
            .run = TEST_RUN_INLINE,
            .flags = TEST_FLAG_NONE,
        };
        test_register(&t);
    }

    {
        test_def_t t = {
            .suite = "CoreNetwork",
            .name = "rdt_transfer",
            .fn = test_rdt_transfer,
            .run = TEST_RUN_INLINE,
            .flags = TEST_FLAG_NONE,
        };
        test_register(&t);
    }

    {
        test_def_t t = {
            .suite = "CoreNetwork",
            .name = "rdt_reject",
            .fn = test_rdt_reject,
            .run = TEST_RUN_INLINE,
            .flags = TEST_FLAG_NONE,
        };
        test_register(&t);
    }

    {
        test_def_t t = {
            .suite = "CoreNetwork",
            .name = "arp_request_build",
            .fn = test_arp_request_build,
            .run = TEST_RUN_INLINE,
            .flags = TEST_FLAG_NONE,
        };
        test_register(&t);
    }

    {
        // TEST_RUN_TASK: arp_resolve is task-context only (it may sleep on a
        // miss), so exercise it from a real task even though this case hits the
        // cached fast path.
        test_def_t t = {
            .suite = "CoreNetwork",
            .name = "arp_resolve_cached",
            .fn = test_arp_resolve_cached,
            .run = TEST_RUN_TASK,
            .flags = TEST_FLAG_NONE,
        };
        test_register(&t);
    }

    {
        test_def_t t = {
            .suite = "CoreNetwork",
            .name = "arp_expiry_logic",
            .fn = test_arp_expiry_logic,
            .run = TEST_RUN_INLINE,
            .flags = TEST_FLAG_NONE,
        };
        test_register(&t);
    }

    {
        test_def_t t = {
            .suite = "CoreNetwork",
            .name = "arp_resolve_timeout",
            .fn = test_arp_resolve_timeout,
            .run = TEST_RUN_TASK,
            .flags = TEST_FLAG_NONE,
        };
        test_register(&t);
    }
}
