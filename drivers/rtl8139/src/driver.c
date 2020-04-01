/**
 * Copyright (c) 2018 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <cardinal/local_spinlock.h>

#include "SysVirtualMemory/vmem.h"
#include "SysPhysicalMemory/phys_mem.h"
#include "CoreNetwork/driver.h"

#include "state.h"
#include "registers.h"

void rtl8139_intr_handler(rtl8139_state_t *state)
{
    while (true)
    {
        local_spinlock_lock(&state->lock);
        if (*(uint16_t *)&state->memar[ISR_REG] & INTR_ROK)
        {
            *(uint16_t *)&state->memar[ISR_REG] = INTR_ROK;
            DEBUG_PRINT("[RTL8139] Receive Interrupt\r\n");
        }

        if (*(uint16_t *)&state->memar[ISR_REG] & INTR_TOK)
        {
            *(uint16_t *)&state->memar[ISR_REG] = INTR_TOK;
            DEBUG_PRINT("[RTL8139] Transmit Interrupt\r\n");
        }
        local_spinlock_unlock(&state->lock);
        halt(); //stop after each iteration to allow other tasks to work, swap for yield later
    }
}

int rtl8139_linkstatus(void *state)
{
    rtl8139_state_t *device = (rtl8139_state_t *)state;
    return ~(device->memar[MEDIA_STATUS_REG] >> 2) & 1;
}

int rtl8139_tx(void *state, void *packet, int len, network_device_tx_flags_t gso)
{
    gso = 0;
    if ((len > TX_DESC_SIZE) || (len <= 0))
        return -1;

    rtl8139_state_t *device = (rtl8139_state_t *)state;
    if ((device->memar[TX_STS_REG(device->free_tx_buf_idx)] & 0xfff) != 0)
        while ((device->memar[TX_STS_REG(device->free_tx_buf_idx)] & (TX_STS_OWN | TX_STS_TOK)) != (TX_STS_OWN | TX_STS_TOK))
            halt();

    local_spinlock_lock(&device->lock);

    memcpy(device->tx_buffer[device->free_tx_buf_idx], packet, len);
    *(uint32_t *)&device->memar[TX_STS_REG(device->free_tx_buf_idx)] = (len & 0xfff); //starts the transmission
    device->free_tx_buf_idx = (device->free_tx_buf_idx + 1) % 4;

    local_spinlock_unlock(&device->lock);
    return 0;
}

static network_device_desc_t device_desc = {
    .name = "RTL8139",
    .features = 0,
    .type = network_device_type_ethernet,
    .handlers.ether = {
        .tx = rtl8139_tx,
        .link_status = rtl8139_linkstatus,
    },
    .spec_features.ether = 0,
    .lock = 0,
};

//Setup this device
int rtl8139_init(rtl8139_state_t *state)
{
    //Power on
    state->memar[CONFIG_1_REG] = 0;

    //Reset
    state->memar[CMD_REG] = CMD_RST_VAL;
    while (state->memar[CMD_REG] & CMD_RST_VAL)
        ;

    //Allocate physical memory for the network buffers
    uintptr_t buffer_phys = pagealloc_alloc(0, 0, physmem_alloc_flags_data | physmem_alloc_flags_zero | physmem_alloc_flags_32bit, RX_BUFFER_SIZE + TX_BUFFER_SIZE);
    intptr_t buffer_virt = vmem_phystovirt((intptr_t)buffer_phys, RX_BUFFER_SIZE + TX_BUFFER_SIZE, vmem_flags_uncached | vmem_flags_kernel | vmem_flags_rw);

    state->rx_buffer_phys = buffer_phys;

    for (int i = 0; i < 4; i++)
        state->tx_buffer_phys[i] = buffer_phys + RX_BUFFER_SIZE + TX_DESC_SIZE * i;

    state->rx_buffer = (uint8_t *)buffer_virt;

    for (int i = 0; i < 4; i++)
        state->tx_buffer[i] = (uint8_t *)(buffer_virt + RX_BUFFER_SIZE + TX_DESC_SIZE * i);

    memset(state->rx_buffer, 0, RX_BUFFER_SIZE + TX_BUFFER_SIZE);

    if (state->rx_buffer_phys & ~0xffffffff)
    {
        DEBUG_PRINT("[RTL8139] Unable to allocate 32-bit physical memory for this device!\r\n");
        return -1;
    }

    //Register to network service
    device_desc.state = (void *)state;
    network_register(&device_desc, &state->net_handle);

    //Start the receiver and transmitter
    state->free_tx_buf_idx = 0;
    state->lock = 0;
    state->memar[CMD_REG] |= CMD_TX_EN | CMD_RX_EN;

    //Set the rx buffer
    *(uint32_t *)&state->memar[RBSTART_REG] = (uint32_t)state->rx_buffer_phys;

    //Set the tx buffers
    for (int i = 0; i < 4; i++)
        *(uint32_t *)&state->memar[TX_ADDR_REG(i)] = (uint32_t)(state->tx_buffer_phys[i]);

    //Configure interrupts
    //*(uint16_t *)&state->memar[IMR_REG] = (INTR_ROK | INTR_TOK | INTR_TIMEOUT);

    //Configure receiver
    *(uint32_t *)&state->memar[RCR_REG] = RCR_RCV_PHYSMATCH | RCR_RCV_MULTICAST | RCR_RCV_BROADCAST | RCR_WRAP | RCR_RX_BUFLEN_64K;

    //Configure transmitter
    *(uint32_t *)&state->memar[TX_CFG_REG] = (*(uint32_t *)&state->memar[TX_CFG_REG] & 0x03080000) /*Preserve IFG*/ | (6 << 8) /*Max DMA Burst = 1024*/;

    return 0;
}