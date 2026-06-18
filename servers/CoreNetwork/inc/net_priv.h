// Copyright (c) 2018 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINALSEMI_NET_PRIV_DRIV_H
#define CARDINALSEMI_NET_PRIV_DRIV_H

#include <stdint.h>
#include "CoreNetwork/driver.h"

// Build an IPv4 address constant in on-wire (network) byte order, suitable for
// direct comparison against the src_ip/dst_ip fields of a received packet (which
// are read from the wire into a uint32_t on this little-endian host).
#define IPV4_ADDR(a, b, c, d) \
    ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

// Default static IPv4 address for an ethernet interface.
//
// TEMPORARY: hardcoded to 10.43.0.116, a free address on the real test LAN, so
// the RTL8111G on the AtomicPi answers ARP / replies to ping end-to-end until a
// DHCP client lands. (Was 10.0.2.15, QEMU slirp's guest address -- restore that,
// or better, make this per-interface configurable, when wiring DHCP.) Per-
// interface addressing / DHCP / a config surface remain future work -- see
// notes/servers/CoreNetwork.md.
#define NET_DEFAULT_IPV4 IPV4_ADDR(10, 43, 0, 116)

typedef struct {
    network_device_type_t type;
    network_device_desc_t device;
    uint8_t mac[6];
    uint32_t ip;  // IPv4 address, network byte order (see IPV4_ADDR)
    int idx;
} interface_def_t;


PRIVATE int ethernet_rx(interface_def_t *interface, void *packet, int len);

PRIVATE int wifi_rx(interface_def_t *interface, void *packet, int len);

// Frame `payload` in an ethernet header destined for `dst_mac` (6 bytes) with
// the given on-wire ethertype, and hand it to the interface's driver for
// transmission. Returns 0 on success, negative on failure.
PRIVATE int ethernet_tx(interface_def_t *interface, const uint8_t *dst_mac,
                        uint16_t ethertype_be, const void *payload, int len);

// Broadcast `packet` (an L3 payload) at L2 with the given on-wire ethertype.
// Used for broadcast protocols (e.g. ARP requests). Unicast L3 transmission
// goes through the per-protocol helpers (e.g. ipv4_tx).
int network_tx_packet(interface_def_t *interface, void *packet, int len, uint16_t protocol_type);

#endif
