// Copyright (c) 2018 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINALSEMI_CORENETWORK_NET_H
#define CARDINALSEMI_CORENETWORK_NET_H

#include <stdint.h>

#define ARP_HEADER_LEN (28)
#define IPV4_HEADER_LEN (20)
#define IPV6_HEADER_LEN (40)

#define UDP_HEADER_LEN (8)

#define UDP_IPV4_PACKET_SPACE (IPV4_HEADER_LEN + UDP_HEADER_LEN)
#define UDP_IPV6_PACKET_SPACE (IPV6_HEADER_LEN + UDP_HEADER_LEN)

#endif