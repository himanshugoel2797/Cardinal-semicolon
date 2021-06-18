/**
 * Copyright (c) 2018 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

#include <stdlib.h>

#include "arp.h"

//TODO: Create a task to run the arp state machine
//TODO: Add a scheduler based lock to reduce lock contention, lock pulls the thread out of the scheduler until it's available

int arp_rx(interface_def_t *interface, void *packet, int len) {
    //TODO: update the arp cache or respond to a request
    interface = NULL;
    len = 0;

    arp_t *pkt = (arp_t *)packet;

    if (TO_LE_FRM_BE_16(pkt->opcode) == ARP_REQUEST)
        DEBUG_PRINT("ARP_REQUEST\r\n");
    else if (TO_LE_FRM_BE_16(pkt->opcode) == ARP_REPLY)
        DEBUG_PRINT("ARP_REPLY\r\n");
    else
        DEBUG_PRINT("ARP_UNKNOWN\r\n");

    return 0;
}