/**
 * Copyright (c) 2018 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

#include <stdint.h>
#include <stdlib.h>
#include <stdlist.h>
#include <cardinal/local_spinlock.h>

#include "net_priv.h"
#include "CoreNetwork/driver.h"

list_t dev_list;
static int dev_list_lock = 0;

list_t interface_list;
static int interface_list_lock = 0;

int devIDs[network_device_type_count];

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