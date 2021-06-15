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

static _Atomic volatile int isr_pending = 0;
void rtl8169_intr_routine(int isr)
{
    isr = 0;
    isr_pending = 1;
}

void rtl8169_intr_handler(rtl8169_state_t *state)
{
    while (true)
    {
        while(!isr_pending)
            halt(); //stop after each iteration to allow other tasks to work, swap for yield later
            
        local_spinlock_lock(&state->lock);
        if (*(uint16_t *)&state->memar[ISR_REG] & INTR_ROK)
        {
            *(uint16_t *)&state->memar[ISR_REG] = INTR_ROK;
            DEBUG_PRINT("[RTL8169] Receive Interrupt\r\n");
        }

        if (*(uint16_t *)&state->memar[ISR_REG] & INTR_TOK)
        {
            *(uint16_t *)&state->memar[ISR_REG] = INTR_TOK;
            DEBUG_PRINT("[RTL8169] Transmit Interrupt\r\n");
        }
        local_spinlock_unlock(&state->lock);
    }
}

int rtl8169_linkstatus(void *state)
{
    rtl8169_state_t *device = (rtl8169_state_t *)state;
    return (device->memar[PHY_STATUS_REG] >> 1) & 1;
}

int rtl8169_tx(void *state, void *packet, int len, network_device_tx_flags_t gso)
{
    gso = 0;
    if ((len > TX_PACKET_SIZE) || (len <= 0))
        return -1;

    rtl8169_state_t *device = (rtl8169_state_t *)state;
    local_spinlock_lock(&device->lock);

    //Wait until the descriptor is available
    while (device->tx_descs[device->free_tx_buf_idx].status.own != 0){
        local_spinlock_unlock(&device->lock);
        halt();
        local_spinlock_lock(&device->lock);
    }

    //Copy the packet over
    memcpy((void*)(device->tx_buffer + device->free_tx_buf_idx * TX_PACKET_SIZE), packet, len);

    //Pass ownership of the descriptor over to the NIC
    device->tx_descs[device->free_tx_buf_idx].cmd.fs = 1;
    device->tx_descs[device->free_tx_buf_idx].cmd.ls = 1;
    device->tx_descs[device->free_tx_buf_idx].cmd.frame_length = len;
    device->tx_descs[device->free_tx_buf_idx].cmd.own = 1;
    device->free_tx_buf_idx = (device->free_tx_buf_idx + 1) % TX_DESC_COUNT;
    
    local_spinlock_unlock(&device->lock);
    return 0;
}

static network_device_desc_t device_desc = {
    .name = "RTL8169",
    .features = 0,
    .type = network_device_type_ethernet,
    .handlers.ether = {
        .tx = rtl8169_tx,
        .link_status = rtl8169_linkstatus,
    },
    .spec_features.ether = 0,
    .lock = 0,
};

//Setup this device
int rtl8169_init(rtl8169_state_t *state)
{
    //Reset
    state->memar[CMD_REG] = CMD_RST_VAL;
    while (state->memar[CMD_REG] & CMD_RST_VAL)
        ;

    //Allocate physical memory for the network buffers
    uintptr_t buffer_phys = pagealloc_alloc(0, 0, physmem_alloc_flags_data | physmem_alloc_flags_zero | physmem_alloc_flags_32bit, RX_BUFFER_SIZE + TX_BUFFER_SIZE);
    intptr_t buffer_virt = vmem_phystovirt((intptr_t)buffer_phys, RX_BUFFER_SIZE + TX_BUFFER_SIZE, vmem_flags_uncached | vmem_flags_kernel | vmem_flags_rw);
    memset((void*)buffer_virt, 0, RX_BUFFER_SIZE + TX_BUFFER_SIZE);

    state->rx_descs_phys = buffer_phys;
    state->rx_buffer_phys = buffer_phys + RX_DESC_REGION_SIZE;
    state->rx_descs = (rtl8169_rx_desc_t *)buffer_virt;
    state->rx_buffer = (uint8_t *)(buffer_virt + RX_DESC_REGION_SIZE);

    state->tx_descs_phys = buffer_phys + RX_BUFFER_SIZE;
    state->tx_buffer_phys = buffer_phys + RX_BUFFER_SIZE + TX_DESC_REGION_SIZE;
    state->tx_descs = (rtl8169_tx_desc_t *)(buffer_virt + RX_BUFFER_SIZE);
    state->tx_buffer = (uint8_t *)(buffer_virt + RX_BUFFER_SIZE + TX_DESC_REGION_SIZE);

    if (state->rx_descs_phys & ~0xffffffff)
    {
        DEBUG_PRINT("[RTL8169] Unable to allocate 32-bit physical memory for this device!\r\n");
        return -1;
    }

    //Setup rx descriptors
    for (int i = 0; i < RX_DESC_COUNT; i++)
    {
        state->rx_descs[i].cmd.own = 1;
        state->rx_descs[i].cmd.buf_size = (RX_PACKET_SIZE & 0x3FFF);
        state->rx_descs[i].cmd.buf_lo = (uint32_t)(state->rx_buffer_phys + RX_PACKET_SIZE * i);
        state->rx_descs[i].cmd.buf_hi = (uint32_t)((state->rx_buffer_phys + RX_PACKET_SIZE * i) >> 32);
    }
    state->rx_descs[RX_DESC_COUNT - 1].cmd.eor = 1;     //Mark the last descriptor as the end of the ring

    //Setup tx descriptors
    for (int i = 0; i < TX_DESC_COUNT; i++)
    {
        state->tx_descs[i].cmd.own = 0;
        state->tx_descs[i].cmd.buf_lo = (uint32_t)(state->tx_buffer_phys + TX_PACKET_SIZE * i);
        state->tx_descs[i].cmd.buf_hi = (uint32_t)((state->tx_buffer_phys + TX_PACKET_SIZE * i) >> 32);
    }
    state->tx_descs[TX_DESC_COUNT - 1].cmd.eor = 1;    //Currently treat the first descriptor as the end of the ring, this will be extended as needed

    //Configure the NIC
    state->memar[_93C56_CMD] = _93C56_UNLOCK; //Unlock config area
    {
        //Configure RX, no minimum rx size, unlimited burst size, accept all packets
        *(uint32_t *)(&state->memar[RCR_REG]) = (7 << 13) | (7 << 8) | (1 << 5) | (1 << 4) | (1 << 3) | (1 << 2) | (1 << 1) | (1 << 0);
        *(uint16_t *)(&state->memar[MAX_RX_PACKET_SIZE_REG]) = RX_PACKET_SIZE;

        //Configure TX, maximum transmit rate, unlimited burst size
        state->memar[CMD_REG] = CMD_TX_EN;
        *(uint32_t *)(&state->memar[TX_CFG_REG]) = (3 << 24) | (7 << 8);
        state->memar[MAX_TX_PACKET_SIZE_REG] = TX_PACKET_SIZE / 128;
        
        //TODO: Enable MSI
    }
    state->memar[_93C56_CMD] = _93C56_LOCK; //Lock config area

    //Register to network service
    device_desc.state = (void *)state;
    network_register(&device_desc, &state->net_handle);

    //Start the receiver and transmitter
    state->free_tx_buf_idx = 0;
    state->lock = 0;

    //Set the rx buffer
    *(uint64_t *)&state->memar[RX_ADDR_REG] = (uint64_t)state->rx_descs_phys;

    //Set the tx buffer
    *(uint64_t *)&state->memar[TX_ADDR_REG] = (uint64_t)state->tx_descs_phys;

    state->memar[CMD_REG] |= CMD_TX_EN | CMD_RX_EN;

    //Configure interrupts
    *(uint16_t *)&state->memar[IMR_REG] = (INTR_ROK | INTR_TOK | INTR_TIMEOUT);
    

    return 0;
}