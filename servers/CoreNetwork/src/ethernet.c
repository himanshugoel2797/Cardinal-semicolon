/**
 * Copyright (c) 2018 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "net_priv.h"
#include "ethernet.h"

#include "arp.h"
#include "ip.h"

// Smallest legal ethernet payload; the frame is zero-padded up to this so a NIC
// that does not pad short frames itself still emits a valid one (FCS excluded --
// the hardware appends that).
#define ETHERNET_MIN_PAYLOAD (60 - (int)sizeof(ethernet_frame_t))

int ethernet_rx(interface_def_t *interface, void *packet, int len) {
    //Decode the ethernet frame and pass it up the stack (to the IP layer?)
    if (len < (int)sizeof(ethernet_frame_t))
        return 0;  // runt: not even a complete ethernet header

    ethernet_frame_t *ether = (ethernet_frame_t*)packet;
    int payload_len = len - (int)sizeof(ethernet_frame_t);

    bool mac_match = true;
    bool broadcast = true;
    for(int i = 0; i < 6; i++) {
        if(ether->dst_mac[i] != interface->mac[i])
            mac_match = false;

        if(ether->dst_mac[i] != 0xff)
            broadcast = false;
    }

    if(mac_match || broadcast) {
        if(ether->type == ETHERNET_TYPE_ARP) {
            //Forward to the arp layer
            arp_rx(interface, ether->src_mac, ether->body, payload_len);
        } else if(ether->type == ETHERNET_TYPE_IPv4) {
            //Forward to the ipv4 layer
            ipv4_rx(interface, ether->src_mac, ether->body, payload_len);
        } else if(ether->type == ETHERNET_TYPE_IPv6) {
            //Forward to the ipv6 layer
            ipv6_rx(interface, ether->src_mac, ether->body, payload_len);
        }
    }

    return 0;
}

int ethernet_tx(interface_def_t *interface, const uint8_t *dst_mac,
                uint16_t ethertype_be, const void *payload, int len) {
    if (len < 0)
        return -1;

    int payload_len = len;
    if (payload_len < ETHERNET_MIN_PAYLOAD)
        payload_len = ETHERNET_MIN_PAYLOAD;

    int frame_len = sizeof(ethernet_frame_t) + payload_len;
    uint8_t *frame = (uint8_t *)malloc(frame_len);
    if (frame == NULL)
        return -1;

    memset(frame, 0, frame_len);  // also zero-pads a short payload
    ethernet_frame_t *eth = (ethernet_frame_t *)frame;
    for (int i = 0; i < 6; i++) {
        eth->dst_mac[i] = dst_mac[i];
        eth->src_mac[i] = interface->mac[i];
    }
    eth->type = ethertype_be;
    memcpy(eth->body, payload, len);

    int ret = -1;
    if (interface->device.handlers.ether.tx != NULL)
        ret = interface->device.handlers.ether.tx(interface->device.state, frame, frame_len, 0);

    free(frame);
    return ret;
}

int wifi_rx(interface_def_t *interface, void *packet, int len) {
    interface = NULL;
    packet = NULL;
    len = 0;

    return 0;
}
