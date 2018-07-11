// Copyright (c) 2018 Himanshu Goel
// 
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINAL_SEMI_CORENETWORK_DRIVER_H
#define CARDINAL_SEMI_CORENETWORK_DRIVER_H

#include <stdint.h>

typedef enum {
    network_device_features_checksum_offload = (1 << 0),
} network_device_features_t;

typedef enum {
    network_device_type_ethernet = 0,
    network_device_type_wifi = 1,
    network_device_type_count,
} network_device_type_t;

typedef enum {
    network_device_tx_flag_ipv4_csum = (1 << 0),
    network_device_tx_flag_tcpv4_csum = (1 << 1),
    network_device_tx_flag_udpv4_csum = (1 << 2),
} network_device_tx_flags_t;

typedef struct {
    int (*tx)(void *state, void *packet, int len, network_device_tx_flags_t flags);
    int (*link_status)(void *state);
} network_ethernet_handlers_t;

typedef enum {
    network_ethernet_features_none = 0,
} network_ethernet_features_t;

typedef struct {
    int (*tx)(void *packet, int len, network_device_tx_flags_t flags);
    //TODO: add scan handlers
    int (*start_scan)();
    int (*finish_scan)();
    int (*abort_scan)();
} network_wifi_handlers_t;

typedef enum {
    network_wifi_features_none = 0,
} network_wifi_features_t;

typedef struct {
    char name[256];

    void *state;

    network_device_features_t features;
    network_device_type_t type;

    uint8_t mac[6];

    union {
        network_ethernet_handlers_t ether;
        network_wifi_handlers_t wifi;
    } handlers;

    union {
        network_ethernet_features_t ether;
        network_wifi_features_t wifi;
    } spec_features;

} network_device_desc_t;

int network_register(network_device_desc_t *desc, void **network_handle);

int network_rx_packet(void *interface_handle, void *packet, int len);

#endif