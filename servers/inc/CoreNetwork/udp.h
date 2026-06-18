// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINAL_SEMI_CORENETWORK_UDP_API_H
#define CARDINAL_SEMI_CORENETWORK_UDP_API_H

#include <stdint.h>

// Public UDP API exported by CoreNetwork. A service binds a destination port
// and is handed every datagram that arrives for it; replies go back out through
// udp_send_to. (The on-wire udp_t struct and the rx demux internals live in the
// server-private inc/udp.h.)

// Handler invoked, in the receive path, for each datagram delivered to a bound
// port. `iface` is the opaque interface handle the datagram arrived on (pass it
// straight back to udp_send_to to reply); `src_ip` is in network byte order and
// `src_mac` is the sender's 6-byte L2 address -- replying to that pair needs no
// ARP resolution. `payload`/`len` are the UDP body. The handler runs in the
// caller's (driver) rx context: it must not block, and the binding lock is
// already released before it is called, so calling udp_send_to from within it is
// safe.
typedef void (*udp_handler_t)(void *ctx, void *iface, uint32_t src_ip,
                              const uint8_t *src_mac, uint16_t src_port,
                              uint16_t dst_port, const void *payload, int len);

// Register `handler` for UDP datagrams whose destination port (host order) is
// `port`. Returns 0 on success, -1 on bad args, a port already bound, or no free
// binding slot.
int udp_bind(uint16_t port, udp_handler_t handler, void *ctx);

// Remove the binding for `port`. Returns 0 if one was removed, -1 otherwise.
int udp_unbind(uint16_t port);

// Send a UDP datagram from `src_port` to `dst_ip`:`dst_port` over `iface`, to the
// L2 address `dst_mac` (typically the src_mac captured from a received datagram,
// so no ARP is needed). The IPv4 + UDP headers and the UDP checksum are built
// here. `port` arguments are host order; `dst_ip` is network order. Returns 0 on
// success, negative on failure.
int udp_send_to(void *iface, uint32_t dst_ip, const uint8_t *dst_mac,
                uint16_t src_port, uint16_t dst_port,
                const void *payload, int len);

#endif
