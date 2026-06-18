// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINALSEMI_CORENETWORK_DHCP_H
#define CARDINALSEMI_CORENETWORK_DHCP_H

#include <stdint.h>
#include <stdbool.h>
#include <types.h>

#include "net_priv.h"

// DHCP client (RFC 2131). A server-private layer that configures an interface's
// address from the network. The protocol logic is split into pure, side-effect-
// free helpers (build / parse / apply) -- unit-tested directly -- and an async
// task + rx handler (dhcp.c) that drives them with retransmission and renewal.

#define DHCP_CLIENT_PORT 68
#define DHCP_SERVER_PORT 67

#define DHCP_OP_BOOTREQUEST 1
#define DHCP_OP_BOOTREPLY   2
#define DHCP_HTYPE_ETHERNET 1
#define DHCP_HLEN_ETHERNET  6

// Magic cookie that precedes the options (RFC 1497), in network byte order.
#define DHCP_MAGIC_COOKIE 0x63825363u
// Broadcast flag (top bit of the 16-bit flags field): ask the server to
// broadcast its reply, since we have no IP yet to receive a unicast.
#define DHCP_FLAG_BROADCAST 0x8000u

// Message types (option 53).
enum {
    DHCP_DISCOVER = 1,
    DHCP_OFFER = 2,
    DHCP_REQUEST = 3,
    DHCP_DECLINE = 4,
    DHCP_ACK = 5,
    DHCP_NAK = 6,
};

// Option codes we build/parse.
enum {
    DHCP_OPT_PAD = 0,
    DHCP_OPT_SUBNET = 1,
    DHCP_OPT_ROUTER = 3,
    DHCP_OPT_DNS = 6,
    DHCP_OPT_REQUESTED_IP = 50,
    DHCP_OPT_LEASE = 51,
    DHCP_OPT_MSGTYPE = 53,
    DHCP_OPT_SERVER_ID = 54,
    DHCP_OPT_PARAM_LIST = 55,
    DHCP_OPT_END = 255,
};

// BOOTP/DHCP fixed header; the variable options[] follow the magic cookie.
typedef struct {
    uint8_t op;
    uint8_t htype;
    uint8_t hlen;
    uint8_t hops;
    uint32_t xid;       // network order
    uint16_t secs;
    uint16_t flags;     // network order (DHCP_FLAG_BROADCAST)
    uint32_t ciaddr;    // client IP (renew); network order
    uint32_t yiaddr;    // "your" (offered/assigned) IP; network order
    uint32_t siaddr;
    uint32_t giaddr;
    uint8_t chaddr[16]; // client hw addr (MAC in the first 6)
    uint8_t sname[64];
    uint8_t file[128];
    uint32_t cookie;    // network order; == TO_BE_FRM_LE_32(DHCP_MAGIC_COOKIE)
    uint8_t options[0];
} PACKED dhcp_msg_t;

// Parsed fields from an OFFER/ACK/NAK. IPv4 values are network byte order (as
// they arrived); lease_secs is host order.
typedef struct {
    uint8_t msg_type;     // DHCP_OFFER / DHCP_ACK / DHCP_NAK
    uint32_t yiaddr;      // assigned address
    uint32_t server_id;   // option 54
    uint32_t netmask;     // option 1
    uint32_t gateway;     // option 3 (first router)
    uint32_t dns;         // option 6 (first server)
    uint32_t lease_secs;  // option 51
} dhcp_result_t;

// --- pure helpers (no I/O, unit-tested) --------------------------------------

// Build a DHCP message of `msg_type` for `mac`/`xid` into `buf` (capacity
// `buflen`). `ciaddr` is the client address (0 except when renewing);
// `requested_ip`/`server_id` (network order, 0 = omit) carry options 50/54 for a
// selecting REQUEST. `broadcast` sets the broadcast flag. Returns the total
// length, or -1 if it would not fit.
int dhcp_build(uint8_t *buf, int buflen, uint8_t msg_type, const uint8_t *mac,
               uint32_t xid, uint32_t ciaddr, uint32_t requested_ip,
               uint32_t server_id, bool broadcast);

// Parse a received datagram as a DHCP reply for our `xid`/`mac`. Returns 0 and
// fills `out` on a well-formed reply addressed to us (with a message type), -1
// otherwise. Treats every length field as untrusted.
int dhcp_parse(const void *buf, int len, uint32_t xid, const uint8_t *mac,
               dhcp_result_t *out);

// Write the acquired configuration into the interface.
void dhcp_apply(interface_def_t *iface, const dhcp_result_t *r);

// --- driver ------------------------------------------------------------------

// Kick off DHCP on `iface` (binds the client port once, spawns the client task).
void dhcp_start(interface_def_t *iface);

#endif
