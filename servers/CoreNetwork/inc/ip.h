// Copyright (c) 2018 Himanshu Goel
// 
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINALSEMI_CORENETWORK_IP_H
#define CARDINALSEMI_CORENETWORK_IP_H

#include <stdint.h>
#include <types.h>

#include "net_priv.h"

int ipv4_rx(interface_def_t *interface, void *packet, int len);
int ipv6_rx(interface_def_t *interface, void *packet, int len);

#endif