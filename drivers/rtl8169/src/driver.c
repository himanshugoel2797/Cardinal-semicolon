/**
 * Copyright (c) 2018 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <cardinal/local_spinlock.h>

#include "SysVirtualMemory/vmem.h"
#include "SysPhysicalMemory/phys_mem.h"
#include "SysTaskMgr/task.h"
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
        task_monitor(task_current(), (uint32_t*)&isr_pending, 0);
        //while (!isr_pending)
        //    task_yield();  //halt(); //stop after each iteration to allow other tasks to work, swap for yield later

        local_spinlock_lock(&state->lock);
        if (*(uint16_t *)&state->memar[ISR_REG] & INTR_ROK)
        {
            //Submit the packet to the network stack
            for (int i = 0; i < RX_DESC_COUNT; i++){
                //Only handle single descriptor packets
                bool avl = (state->rx_descs[i].status.own == 0);
                if (!avl)
                    continue;

                //if (state->rx_descs[i].status.fs && state->rx_descs[i].status.ls)
                network_rx_packet(state->net_handle, state->rx_buffer + RX_PACKET_SIZE * i, state->rx_descs[i].status.frame_length);
                
                //Return this descriptor to the NIC
                state->rx_descs[i].cmd.frame_length = (RX_PACKET_SIZE & 0x3FFF);
                state->rx_descs[i].cmd.rsvd0 = 0;
                state->rx_descs[i].cmd.rsvd1 = 0;
                state->rx_descs[i].cmd.own = 1;
            }
            //DEBUG_PRINT("[RTL8169] Receive Interrupt\r\n");

            *(uint16_t *)&state->memar[ISR_REG] = INTR_ROK;
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
    uintptr_t buffer_phys = pagealloc_alloc(0, 0, physmem_alloc_flags_data, RX_BUFFER_SIZE + TX_BUFFER_SIZE);
    intptr_t buffer_virt = vmem_phystovirt((intptr_t)buffer_phys, RX_BUFFER_SIZE + TX_BUFFER_SIZE, vmem_flags_uncached | vmem_flags_kernel | vmem_flags_rw);
    memset((void*)buffer_virt, 0, RX_BUFFER_SIZE + TX_BUFFER_SIZE);

    state->free_tx_buf_idx = 0;
    state->lock = 0;

    state->rx_descs_phys = buffer_phys;
    state->rx_buffer_phys = buffer_phys + RX_DESC_REGION_SIZE;
    state->rx_descs = (rtl8169_rx_desc_t *)buffer_virt;
    state->rx_buffer = (uint8_t *)(buffer_virt + RX_DESC_REGION_SIZE);

    state->tx_descs_phys = buffer_phys + RX_BUFFER_SIZE;
    state->tx_buffer_phys = buffer_phys + RX_BUFFER_SIZE + TX_DESC_REGION_SIZE;
    state->tx_descs = (rtl8169_tx_desc_t *)(buffer_virt + RX_BUFFER_SIZE);
    state->tx_buffer = (uint8_t *)(buffer_virt + RX_BUFFER_SIZE + TX_DESC_REGION_SIZE);

    //Setup rx descriptors
    for (int i = 0; i < RX_DESC_COUNT; i++)
    {
        state->rx_descs[i].cmd.frame_length = (RX_PACKET_SIZE & 0x3FFF);
        state->rx_descs[i].cmd.buf_lo = (uint32_t)(state->rx_buffer_phys + RX_PACKET_SIZE * i);
        state->rx_descs[i].cmd.buf_hi = (uint32_t)((state->rx_buffer_phys + RX_PACKET_SIZE * i) >> 32);
        state->rx_descs[i].cmd.own = 1;
    }
    state->rx_descs[RX_DESC_COUNT - 1].cmd.eor = 1;     //Mark the last descriptor as the end of the ring

    //Setup tx descriptors
    for (int i = 0; i < TX_DESC_COUNT; i++)
    {
        state->tx_descs[i].cmd.buf_lo = (uint32_t)(state->tx_buffer_phys + TX_PACKET_SIZE * i);
        state->tx_descs[i].cmd.buf_hi = (uint32_t)((state->tx_buffer_phys + TX_PACKET_SIZE * i) >> 32);
        state->tx_descs[i].cmd.own = 0;
    }
    state->tx_descs[TX_DESC_COUNT - 1].cmd.eor = 1;

    for (int i = 0; i < 6; i++)
        device_desc.mac[i] = state->memar[MAC_REG(i)];

    *(uint16_t *)(&state->memar[0x00e0]) |= 3;

    //Configure the NIC
    state->memar[_93C56_CMD] = _93C56_UNLOCK; //Unlock config area
    {
    }
    state->memar[_93C56_CMD] = _93C56_LOCK; //Lock config area

        //*(uint32_t *)(&state->memar[MAR_REG(6)]) = 0xffffffff;
        //*(uint32_t *)(&state->memar[MAR_REG(4)]) = 0xffffffff;
        
        //TODO: Newer chips don't need the TX to be on before configuring
        //state->memar[CMD_REG] = CMD_TX_EN | CMD_RX_EN; 

        //Configure RX, no minimum rx size, unlimited burst size, accept all packets
        *(uint32_t *)(&state->memar[RCR_REG]) = (7 << 13) | (7 << 8) | (1 << 5) | (1 << 4) | (1 << 3) | (1 << 2) | (1 << 1) | (1 << 0);
        
        //Configure TX, maximum transmit rate, unlimited burst size
        *(uint32_t *)(&state->memar[TX_CFG_REG]) = (3 << 24) | (7 << 8);
        state->memar[MAX_TX_PACKET_SIZE_REG] = TX_PACKET_SIZE / 128;

        //Set the rx buffer
        *(uint32_t *)&state->memar[RX_ADDR_REG] = (uint32_t)state->rx_descs_phys;
        *(uint32_t *)&state->memar[RX_ADDR_REG + 4] = (uint32_t)(state->rx_descs_phys >> 32);

        //Set the tx buffer
        *(uint32_t *)&state->memar[TX_ADDR_REG] = (uint32_t)state->tx_descs_phys;
        *(uint32_t *)&state->memar[TX_ADDR_REG + 4] = (uint32_t)(state->tx_descs_phys >> 32);

        //Disable RXDV gate
        *(uint32_t *)&state->memar[MISC_REG] &= ~0x00080000;

        //Register to network service
        device_desc.state = (void *)state;
        network_register(&device_desc, &state->net_handle);
        
        *(uint16_t *)(&state->memar[0x00e2]) |= 0x5100;

        //Start the receiver and transmitter
        state->memar[CMD_REG] = CMD_TX_EN | CMD_RX_EN;

        //Configure interrupts
        *(uint16_t *)&state->memar[IMR_REG] = (INTR_ROK | INTR_TOK);
        *(uint16_t *)&state->memar[ISR_REG] = (INTR_ROK | INTR_TOK);
   
        *(uint32_t *)&state->memar[MISSEDPKT_REG] = 0;
        *(uint16_t *)(&state->memar[MAX_RX_PACKET_SIZE_REG]) = RX_PACKET_SIZE;
        state->memar[CONFIG_1_REG] |= 0x20; //Driver loaded
    return 0;
}