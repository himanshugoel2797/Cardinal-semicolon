// Copyright (c) 2018 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINAL_SEMI_RTL8139_STATE_H
#define CARDINAL_SEMI_RTL8139_STATE_H

#include <stdint.h>
#include "CoreNetwork/driver.h"

typedef struct {
    uint32_t frame_length : 16;
    uint32_t tcpcs : 1; //0
    uint32_t udpcs : 1; //0
    uint32_t ipcs : 1; //0
    uint32_t rsvd0 : 8;
    uint32_t lgsen : 1; //0
    uint32_t ls : 1;
    uint32_t fs : 1;
    uint32_t eor : 1;
    uint32_t own : 1;//1
    uint32_t rsvd1; //0
    uint32_t buf_lo;
    uint32_t buf_hi;
} rtl8169_tx_normal_desc_t;

typedef struct {
    uint32_t rsvd0 : 28;
    uint32_t ls : 1;
    uint32_t fs : 1;
    uint32_t eor : 1;
    uint32_t own : 1;//0
    uint32_t rsvd1;
    uint32_t buf_lo;
    uint32_t buf_hi;
} rtl8169_tx_status_t;

typedef struct {
    union{
        rtl8169_tx_normal_desc_t cmd;
        rtl8169_tx_status_t status;
    };
} rtl8169_tx_desc_t;

typedef struct {
    uint32_t buf_size : 14;
    uint32_t rsvd0 : 16;
    uint32_t eor : 1;
    uint32_t own : 1;//1
    uint32_t rsvd1;
    uint32_t buf_lo;
    uint32_t buf_hi;
} rtl8169_rx_cmd_t;

typedef struct {
    uint32_t frame_length : 14;
    uint32_t tcpf : 1;
    uint32_t udpf : 1;
    uint32_t ipf : 1;
    uint32_t pid0 : 1;
    uint32_t pid1 : 1;
    uint32_t crc : 1;
    uint32_t runt : 1;
    uint32_t res : 1;
    uint32_t rwt : 1;
    uint32_t rsvd0 : 2;
    uint32_t bar : 1;
    uint32_t pam : 1;
    uint32_t mar : 1;
    uint32_t ls : 1;
    uint32_t fs : 1;
    uint32_t eor : 1;
    uint32_t own : 1;//0
    uint32_t rsvd1;
    uint32_t buf_lo;
    uint32_t buf_hi;
} rtl8169_rx_status_t;

typedef struct {
    union{
        rtl8169_rx_cmd_t cmd;
        rtl8169_rx_status_t status;
    };
} rtl8169_rx_desc_t;

#define TX_DESC_SIZE sizeof(rtl8169_tx_desc_t)
#define TX_DESC_COUNT 1024
#define TX_PACKET_SIZE 2048
#define TX_BUFFER_SIZE ((TX_DESC_SIZE + TX_PACKET_SIZE) * TX_DESC_COUNT)
#define TX_DESC_REGION_SIZE (TX_DESC_SIZE * TX_DESC_COUNT)

#define RX_DESC_SIZE sizeof(rtl8169_rx_desc_t)
#define RX_DESC_COUNT 1024
#define RX_PACKET_SIZE 2048
#define RX_BUFFER_SIZE ((RX_DESC_SIZE + RX_PACKET_SIZE) * RX_DESC_COUNT)
#define RX_DESC_REGION_SIZE (RX_DESC_SIZE * RX_DESC_COUNT)


typedef struct
{
    volatile uint8_t *memar;

    uintptr_t rx_buffer_phys;
    uintptr_t tx_buffer_phys;

    volatile uint8_t *rx_buffer;
    volatile uint8_t *tx_buffer;

    uintptr_t rx_descs_phys;
    uintptr_t tx_descs_phys;

    volatile rtl8169_tx_desc_t *tx_descs;
    volatile rtl8169_rx_desc_t *rx_descs;

    volatile uint16_t free_tx_buf_idx;
    void *net_handle;
    int lock;
} rtl8169_state_t;

void rtl8169_intr_handler(rtl8169_state_t *state);
void rtl8169_intr_routine(int isr);
int rtl8169_init(rtl8169_state_t *state);

#endif