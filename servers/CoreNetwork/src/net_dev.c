/**
 * Copyright (c) 2018 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

#include <stdint.h>
#include <stdlib.h>
#include <stdlist.h>
#include <string.h>
#include <cardinal/local_spinlock.h>

#include "net_priv.h"
#include "CoreNetwork/driver.h"
#include "ethernet.h"
#include "arp.h"
#include "ip.h"
#include "icmp.h"
#include "checksum.h"

static list_t dev_list;
static int dev_list_lock = 0;

static list_t interface_list;
static int interface_list_lock = 0;

static int devIDs[network_device_type_count];

PRIVATE int network_init(void)
{
    list_init(&dev_list);
    list_init(&interface_list);

    for (int i = 0; i < network_device_type_count; i++)
        devIDs[i] = 0;

    return 0;
}

int network_register(network_device_desc_t *desc, void **network_handle)
{

    network_device_type_t devType;
    uint8_t mac[6];

    local_spinlock_lock(&dev_list_lock);
    local_spinlock_lock(&desc->lock);
    {
        list_append(&dev_list, desc);

        devType = desc->type;
        for (int i = 0; i < 6; i++)
            mac[i] = desc->mac[i];

        DEBUG_PRINT("[CoreNetwork] Registered Device: ");
        DEBUG_PRINT(desc->name);
        DEBUG_PRINT("\r\n");
    }
    local_spinlock_unlock(&desc->lock);
    local_spinlock_unlock(&dev_list_lock);

    local_spinlock_lock(&interface_list_lock);
    {
        interface_def_t *def = (interface_def_t *)malloc(sizeof(interface_def_t));
        def->type = devType;
        def->device = *desc;
        def->idx = devIDs[def->type]++;
        def->ip = NET_DEFAULT_IPV4;
        for (int i = 0; i < 6; i++)
            def->mac[i] = mac[i];

        *network_handle = def;

        list_append(&interface_list, def);
    }
    local_spinlock_unlock(&interface_list_lock);

    return 0;
}

//Can be called from any thread, make sure it's thread safe
int network_rx_packet(void *interface_handle, void *packet, int len)
{
    interface_def_t *def = (interface_def_t *)interface_handle;

    //Process this packet
    switch (def->type)
    {
    case network_device_type_ethernet:
        return ethernet_rx(def, packet, len);
    case network_device_type_wifi:
        return wifi_rx(def, packet, len);
    default:
        DEBUG_PRINT("[CoreNetwork] Network RX device type unknown.\r\n");
        return -1;
    }

    return 0;
}

// Broadcast an L3 payload at L2 with the given on-wire ethertype. Used by
// broadcast protocols (e.g. ARP requests); unicast L3 transmission goes through
// the per-protocol helpers (ipv4_tx). The richer tx path -- per-device queues, a
// tx thread, and outbound ARP resolution with packet hold/retry -- is a deferred
// design decision (see notes/servers/CoreNetwork.md).
int network_tx_packet(interface_def_t *interface, void *packet, int len, uint16_t protocol_type)
{
    static const uint8_t broadcast_mac[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

    if (interface->type != network_device_type_ethernet)
        return -1;

    return ethernet_tx(interface, broadcast_mac, protocol_type, packet, len);
}

// =========================================================================
// TEMP: network bring-up self-test. Throwaway exploration glue, NOT a real
// interface -- delete once a proper socket/test API exists (see
// notes/servers/CoreNetwork.md). Invoked once from servicescript.txt via
// `CALL:network_debug_selftest`. It ARPs the QEMU slirp gateway (10.0.2.2) and
// then pings it, so a packet capture can validate the tx/framing/checksum and rx
// paths against a real peer.
// =========================================================================
void network_debug_selftest(void) {
    uint32_t gw_ip = IPV4_ADDR(10, 0, 2, 2);

    local_spinlock_lock(&interface_list_lock);
    interface_def_t *def = (list_len(&interface_list) > 0)
                               ? (interface_def_t *)list_at(&interface_list, 0)
                               : NULL;
    local_spinlock_unlock(&interface_list_lock);

    if (def == NULL || def->type != network_device_type_ethernet) {
        DEBUG_PRINT("[CoreNetwork] selftest: no ethernet interface\r\n");
        return;
    }

    // 1) Broadcast an ARP request for the gateway.
    DEBUG_PRINT("[CoreNetwork] selftest: ARP who-has 10.0.2.2\r\n");
    arp_t req;
    req.hw_type = TO_BE_FRM_LE_16(ARP_HW_TYPE_ETHERNET);
    req.protocol_type = TO_BE_FRM_LE_16(ARP_PROTO_TYPE_IPV4);
    req.hw_addr_len = 6;
    req.protocol_addr_len = 4;
    req.opcode = TO_BE_FRM_LE_16(ARP_REQUEST);
    for (int i = 0; i < 6; i++) {
        req.src_mac[i] = def->mac[i];
        req.dst_mac[i] = 0;
    }
    req.src_ip = def->ip;
    req.dst_ip = gw_ip;
    network_tx_packet(def, &req, sizeof(req), ETHERNET_TYPE_ARP);

    // 2) Wait (bounded) for the reply to be learned, then ping the gateway.
    //    No task_yield import here; the bounded busy-spin is long enough that the
    //    preemption timer schedules the driver's rx task in between.
    uint8_t gw_mac[6];
    int resolved = 0;
    for (int attempt = 0; attempt < 200 && !resolved; attempt++) {
        for (volatile uint64_t d = 0; d < 2000000; d++)
            ;
        resolved = arp_cache_lookup(gw_ip, gw_mac);
    }

    if (!resolved) {
        DEBUG_PRINT("[CoreNetwork] selftest: gateway not resolved (check capture for our ARP request)\r\n");
        return;
    }

    DEBUG_PRINT("[CoreNetwork] selftest: gateway resolved, sending ICMP echo\r\n");
    uint8_t echo[16];
    memset(echo, 0, sizeof(echo));
    icmp_t *icmp = (icmp_t *)echo;
    icmp->type = ICMP_TYPE_ECHO_REQUEST;
    icmp->code = 0;
    echo[4] = 0x12;  // identifier
    echo[5] = 0x34;
    echo[6] = 0x00;  // sequence
    echo[7] = 0x01;
    for (int i = 8; i < 16; i++)
        echo[i] = (uint8_t)i;  // payload
    icmp->csum = 0;
    icmp->csum = net_checksum16(echo, sizeof(echo));
    ipv4_tx(def, gw_ip, gw_mac, IP_PROTOCOL_ICMP, echo, sizeof(echo));
}