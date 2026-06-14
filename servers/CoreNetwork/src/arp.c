/**
 * Copyright (c) 2018 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

#include <stdlib.h>
#include <string.h>
#include <cardinal/local_spinlock.h>

#include "arp.h"
#include "ethernet.h"

// A small fixed ARP cache. Entries are learned from observed traffic and
// overwritten round-robin once full -- adequate for the current single-subnet
// bring-up. A timed-aging state machine is deferred (see
// notes/servers/CoreNetwork.md).
#define ARP_CACHE_SIZE 32

typedef struct {
    uint32_t ip;  // network byte order
    uint8_t mac[6];
    bool valid;
} arp_entry_t;

static arp_entry_t arp_cache[ARP_CACHE_SIZE];
static int arp_cache_next = 0;
static int arp_cache_lock = 0;

void arp_cache_update(uint32_t ip, const uint8_t *mac) {
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

    local_spinlock_unlock(&arp_cache_lock);
}

bool arp_cache_lookup(uint32_t ip, uint8_t *mac_out) {
    bool found = false;
    local_spinlock_lock(&arp_cache_lock);

    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
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
