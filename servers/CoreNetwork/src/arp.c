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
    packet = NULL;
    len = 0;

    DEBUG_PRINT("ARP\r\n");

    return 0;
}