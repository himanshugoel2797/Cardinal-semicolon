/**
 * Copyright (c) 2018 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdlist.h>
#include <cardinal/local_spinlock.h>

#include "boot_information.h"
#include "net_priv.h"
#include "dhcp.h"
#include "CoreNetwork/driver.h"
#include "SysTest/test.h"

// If the kernel command line pins a static address with "cardinal.ip=A.B.C.D",
// parse it into *out (network order) and return true; otherwise return false, in
// which case the interface runs DHCP instead. Every length is validated, so a
// malformed value falls through to DHCP rather than producing a garbage address.
static bool cmdline_static_ip(uint32_t *out) {
    CardinalBootInfo *bi = GetBootInfo();
    if (bi == NULL)
        return false;

    const char *s = strstr(bi->Cmdline, "cardinal.ip=");
    if (s == NULL)
        return false;
    s += sizeof("cardinal.ip=") - 1;

    uint32_t oct[4] = {0, 0, 0, 0};
    int n = 0;
    while (n < 4) {
        if (*s < '0' || *s > '9')
            break;  // each octet must start with a digit
        uint32_t v = 0;
        int digits = 0;
        while (*s >= '0' && *s <= '9' && digits < 3) {
            v = v * 10 + (uint32_t)(*s - '0');
            s++;
            digits++;
        }
        if (v > 255)
            return false;  // not a valid octet -> reject the whole quad
        oct[n++] = v;
        if (n < 4) {
            if (*s != '.')
                break;  // malformed -> reject the whole thing
            s++;
        }
    }
    if (n != 4)
        return false;  // not a complete dotted quad
    *out = IPV4_ADDR(oct[0], oct[1], oct[2], oct[3]);
    return true;
}

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

    // A static cmdline address skips DHCP; otherwise the interface comes up at
    // 0.0.0.0 and a DHCP client (started below, after the list lock is released)
    // fills in the address/netmask/gateway/DNS.
    uint32_t static_ip = 0;
    bool use_dhcp = !cmdline_static_ip(&static_ip);

    interface_def_t *def = (interface_def_t *)malloc(sizeof(interface_def_t));
    if (def == NULL)
        return -1;

    local_spinlock_lock(&interface_list_lock);
    {
        def->type = devType;
        def->device = *desc;
        def->idx = devIDs[def->type]++;
        def->ip = use_dhcp ? 0 : static_ip;
        def->netmask = 0;
        def->gateway = 0;
        def->dns = 0;
        for (int i = 0; i < 6; i++)
            def->mac[i] = mac[i];

        *network_handle = def;

        list_append(&interface_list, def);
    }
    local_spinlock_unlock(&interface_list_lock);

    // Spawn DHCP outside the list lock (it binds a UDP port and creates a task).
    // Skipped under SysTest: the mock interfaces registered by the tests must not
    // spin up a background DHCP task that binds port 68 and races the tests' tx
    // capture -- the DHCP protocol logic is covered by direct unit tests of the
    // pure helpers instead.
    if (use_dhcp && !test_mode_active())
        dhcp_start(def);

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