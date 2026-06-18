// Copyright (c) 2018 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINALSEMI_CORENETWORK_ARP_H
#define CARDINALSEMI_CORENETWORK_ARP_H

#include <stdint.h>
#include <stdbool.h>
#include <types.h>

#include "net_priv.h"

#define ARP_HW_TYPE_ETHERNET (1)
#define ARP_PROTO_TYPE_IPV4 (0x0800)

#define ARP_REQUEST 0x0001
#define ARP_REPLY 0x0002

// How long a learned cache entry stays valid before lookups treat it as a miss
// and re-resolve. An entry stamped with expiry 0 never ages (the fallback when
// no monotonic counter is calibrated -- see arp_cache_update).
#define ARP_CACHE_TTL_NS SEC(120)

// Pure expiry predicate, factored out so the aging logic is unit-testable
// without a real clock. `now`/`expiry` are timer_timestamp_ns()-domain
// nanoseconds; expiry 0 means "never expires".
static inline bool arp_entry_expired(uint64_t expiry, uint64_t now) {
    return expiry != 0 && now >= expiry;
}

typedef struct {
    uint16_t hw_type;
    uint16_t protocol_type;
    uint8_t hw_addr_len;
    uint8_t protocol_addr_len;
    uint16_t opcode;
    uint8_t src_mac[6];
    uint32_t src_ip;
    uint8_t dst_mac[6];
    uint32_t dst_ip;
} PACKED arp_t;

int arp_rx(interface_def_t *interface, const uint8_t *src_mac, void *packet, int len);

// Insert or refresh an IPv4 (network-order) -> MAC mapping in the ARP cache.
void arp_cache_update(uint32_t ip, const uint8_t *mac);

// Look up the MAC for an IPv4 address (network order). Writes 6 bytes into
// `mac_out` and returns true on hit (a non-expired entry), false on miss. An
// expired entry is treated as a miss and reclaimed.
bool arp_cache_lookup(uint32_t ip, uint8_t *mac_out);

// Broadcast an ARP request asking who owns `target_ip` (network order) on this
// interface. The reply is learned passively by arp_rx into the cache.
void arp_send_request(interface_def_t *interface, uint32_t target_ip);

// Resolve `ip` (network order) to a MAC, writing 6 bytes into `mac_out`. Returns
// 0 on success, -1 on timeout. On a cache miss this broadcasts ARP requests and
// polls the cache, so it MUST be called from a task context (it sleeps) -- never
// from an rx/interrupt path. The caller supplies the next-hop IP (on-link host
// or gateway); routing is the caller's concern, not ARP's.
int arp_resolve(interface_def_t *interface, uint32_t ip, uint8_t *mac_out);

#endif
