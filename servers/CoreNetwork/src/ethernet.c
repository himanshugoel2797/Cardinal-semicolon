/**
 * Copyright (c) 2018 Himanshu Goel
 * 
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

#include <stdint.h>
#include <stdlib.h>

#include "net_priv.h"
#include "ethernet.h"
#include "arp.h"

int ethernet_rx(interface_def_t *interface, void *packet, int len) {
    len = 0;

    //Decode the ethernet frame and pass it up the stack (to the IP layer?)
    ethernet_frame_t *ether = (ethernet_frame_t*)packet;

    bool mac_match = true;
    for(int i = 0; i < 6; i++)
        if(ether->dst_mac[i] != interface->mac[i])
            mac_match = false;

    if(mac_match) {
        DEBUG_PRINT("Packet RX\r\n");

        if(ether->type == ETHERNET_TYPE_ARP) {
            //Forward to the arp layer
            arp_rx(interface, ether->body, len - sizeof(ethernet_frame_t));
        }else if(ether->type == ETHERNET_TYPE_IPv4) {
            //Forward to the ipv4 layer
            ipv4_rx(interface, ether->body, len - sizeof(ethernet_frame_t));
        }else if(ether->type == ETHERNET_TYPE_IPv6) {
            DEBUG_PRINT("IPv6 Unsupported\r\n");
            //Forward to the ipv6 layer
            ipv6_rx(interface, ether->body, len - sizeof(ethernet_frame_t));
        }
    }

    return 0;
}

int wifi_rx(interface_def_t *interface, void *packet, int len) {
    interface = NULL;
    packet = NULL;
    len = 0;

    return 0;
}