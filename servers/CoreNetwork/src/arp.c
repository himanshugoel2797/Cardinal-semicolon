/**
 * Copyright (c) 2018 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

#include <stdlib.h>
#include <string.h>
#include <cardinal/local_spinlock.h>
#include <SysTaskMgr/task.h>
#include <SysTimer/timer.h>

#include "arp.h"
#include "ethernet.h"

// A small fixed ARP cache. Entries are learned from observed traffic and
// overwritten round-robin once full -- adequate for the current single-subnet
// bring-up. Entries age out after ARP_CACHE_TTL_NS so a stale mapping does not
// persist indefinitely (lazy: expired entries are dropped on lookup, no sweep).
#define ARP_CACHE_SIZE 32

typedef struct {
    uint32_t ip;  // network byte order
    uint8_t mac[6];
    bool valid;
    uint64_t expiry_ns;  // timer_timestamp_ns() domain; 0 = never expires
} arp_entry_t;

static arp_entry_t arp_cache[ARP_CACHE_SIZE];
static int arp_cache_next = 0;
static int arp_cache_lock = 0;

// Current monotonic time, or 0 if no counter is calibrated. 0 doubles as the
// "never expires" stamp, so a missing clock simply disables aging rather than
// expiring everything instantly.
static uint64_t arp_now(void) {
    uint64_t t = timer_timestamp_ns();
    return (t == TIMER_NO_COUNTER) ? 0 : t;
}

void arp_cache_update(uint32_t ip, const uint8_t *mac) {
    uint64_t now = arp_now();
    local_spinlock_lock(&arp_cache_lock);

    int slot = -1;
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {  // not present: take a free slot, else evict round-robin
        for (int i = 0; i < ARP_CACHE_SIZE; i++) {
            if (!arp_cache[i].valid) {
                slot = i;
                break;
            }
        }
        if (slot < 0) {
            slot = arp_cache_next;
            arp_cache_next = (arp_cache_next + 1) % ARP_CACHE_SIZE;
        }
    }

    arp_cache[slot].ip = ip;
    for (int i = 0; i < 6; i++)
        arp_cache[slot].mac[i] = mac[i];
    arp_cache[slot].valid = true;
    // expiry 0 = never (no calibrated counter); else now + TTL, saturating so a
    // near-max clock can't wrap to a past time and instantly expire.
    uint64_t max_ns = (uint64_t)-1;
    arp_cache[slot].expiry_ns =
        now ? ((now > max_ns - ARP_CACHE_TTL_NS) ? max_ns : now + ARP_CACHE_TTL_NS)
            : 0;

    local_spinlock_unlock(&arp_cache_lock);
}

bool arp_cache_lookup(uint32_t ip, uint8_t *mac_out) {
    bool found = false;
    uint64_t now = arp_now();
    local_spinlock_lock(&arp_cache_lock);

    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            if (arp_entry_expired(arp_cache[i].expiry_ns, now)) {
                arp_cache[i].valid = false;  // stale -> reclaim, report a miss
                break;
            }
            for (int j = 0; j < 6; j++)
                mac_out[j] = arp_cache[i].mac[j];
            found = true;
            break;
        }
    }

    local_spinlock_unlock(&arp_cache_lock);
    return found;
}

static void arp_send_reply(interface_def_t *interface, arp_t *request) {
    arp_t reply;
    reply.hw_type = TO_BE_FRM_LE_16(ARP_HW_TYPE_ETHERNET);
    reply.protocol_type = TO_BE_FRM_LE_16(ARP_PROTO_TYPE_IPV4);
    reply.hw_addr_len = 6;
    reply.protocol_addr_len = 4;
    reply.opcode = TO_BE_FRM_LE_16(ARP_REPLY);
    for (int i = 0; i < 6; i++) {
        reply.src_mac[i] = interface->mac[i];
        reply.dst_mac[i] = request->src_mac[i];
    }
    reply.src_ip = interface->ip;
    reply.dst_ip = request->src_ip;

    ethernet_tx(interface, request->src_mac, ETHERNET_TYPE_ARP, &reply, sizeof(arp_t));
}

int arp_rx(interface_def_t *interface, const uint8_t *src_mac, void *packet, int len) {
    src_mac = NULL;  // ARP carries its own sender hardware address; ignore the L2 one
    if (len < (int)sizeof(arp_t))
        return 0;  // too short to be a complete ARP packet

    arp_t *pkt = (arp_t *)packet;

    // Only handle ethernet/IPv4 ARP.
    if (pkt->hw_type != TO_BE_FRM_LE_16(ARP_HW_TYPE_ETHERNET) ||
        pkt->protocol_type != TO_BE_FRM_LE_16(ARP_PROTO_TYPE_IPV4) ||
        pkt->hw_addr_len != 6 || pkt->protocol_addr_len != 4)
        return 0;

    // Learn the sender regardless of opcode.
    arp_cache_update(pkt->src_ip, pkt->src_mac);

    uint16_t opcode = TO_LE_FRM_BE_16(pkt->opcode);
    if (opcode == ARP_REQUEST && pkt->dst_ip == interface->ip)
        arp_send_reply(interface, pkt);

    return 0;
}

void arp_send_request(interface_def_t *interface, uint32_t target_ip) {
    static const uint8_t broadcast_mac[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

    arp_t req;
    req.hw_type = TO_BE_FRM_LE_16(ARP_HW_TYPE_ETHERNET);
    req.protocol_type = TO_BE_FRM_LE_16(ARP_PROTO_TYPE_IPV4);
    req.hw_addr_len = 6;
    req.protocol_addr_len = 4;
    req.opcode = TO_BE_FRM_LE_16(ARP_REQUEST);
    for (int i = 0; i < 6; i++) {
        req.src_mac[i] = interface->mac[i];
        req.dst_mac[i] = 0;  // unknown -- that is what we are asking for
    }
    req.src_ip = interface->ip;
    req.dst_ip = target_ip;

    ethernet_tx(interface, broadcast_mac, ETHERNET_TYPE_ARP, &req, sizeof(arp_t));
}

// Number of request broadcasts and the per-request poll window. ~4 requests over
// ~1s before giving up -- ample for a healthy LAN, bounded for a dead host.
#define ARP_RESOLVE_TRIES 4
#define ARP_RESOLVE_WINDOW_NS MS(250)
#define ARP_RESOLVE_POLL_NS MS(20)

int arp_resolve(interface_def_t *interface, uint32_t ip, uint8_t *mac_out) {
    // Fast path: a fresh cache entry needs no traffic.
    if (arp_cache_lookup(ip, mac_out))
        return 0;

    // Slow path: broadcast a request and poll the cache, which arp_rx fills when
    // the reply arrives. Sleeps, so this must run in a task context.
    cs_id self = task_current();
    for (int t = 0; t < ARP_RESOLVE_TRIES; t++) {
        arp_send_request(interface, ip);

        uint64_t waited = 0;
        while (waited < ARP_RESOLVE_WINDOW_NS) {
            task_sleep(self, ARP_RESOLVE_POLL_NS);
            waited += ARP_RESOLVE_POLL_NS;
            if (arp_cache_lookup(ip, mac_out))
                return 0;
        }
    }
    return -1;
}
