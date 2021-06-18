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
        def->device = desc;
        def->idx = devIDs[def->type]++;
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

//Switch to allocation pattern? Read up on network stack designs
int network_tx_packet(interface_def_t *interface, void *packet, int len, uint16_t protocol_type)
{
    //TODO: build a transmission frame around the packet based on the interface type and submit to the driver for transmission
    interface = NULL;
    packet = NULL;
    len = 0;
    protocol_type = 0;

    return 0;
}