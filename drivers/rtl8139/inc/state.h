// Copyright (c) 2018 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINAL_SEMI_RTL8139_STATE_H
#define CARDINAL_SEMI_RTL8139_STATE_H

#include <stdint.h>
#include "CoreNetwork/driver.h"

#define RX_BUFFER_SIZE (68 * 1024)
#define TX_BUFFER_SIZE (8 * 1024)
#define TX_DESC_SIZE (2 * 1024)

typedef struct
{
    volatile uint8_t *memar;

    uint8_t *rx_buffer;
    uint8_t *tx_buffer[4];

    uintptr_t rx_buffer_phys;
    uintptr_t tx_buffer_phys[4];

    uint8_t free_tx_buf_idx;
    void *net_handle;
    int lock;
} rtl8139_state_t;

void rtl8139_intr_handler(rtl8139_state_t *state);
int rtl8139_init(rtl8139_state_t *state);

#endif