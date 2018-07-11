// Copyright (c) 2018 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINALSEMI_NET_PRIV_DRIV_H
#define CARDINALSEMI_NET_PRIV_DRIV_H

#include <stdint.h>
#include "CoreNetwork/driver.h"

typedef struct {
    network_device_type_t type;
    network_device_desc_t *device;
    uint8_t mac[6];
    int idx;
} interface_def_t;

PRIVATE int network_init(void);

PRIVATE int ethernet_rx(interface_def_t *interface, void *packet, int len);

PRIVATE int wifi_rx(interface_def_t *interface, void *packet, int len);

int network_tx_packet(interface_def_t *interface, void *packet, int len, uint16_t protocol_type);

#endif