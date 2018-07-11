/**
 * Copyright (c) 2018 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

#include <stdint.h>
#include <string.h>

#include "SysVirtualMemory/vmem.h"
#include "SysPhysicalMemory/phys_mem.h"

#include "state.h"
#include "registers.h"

void rtl8139_intr_handler(int int_num) {
    int_num = 0;
    DEBUG_PRINT("Network Interrupt\r\n");
}

//Setup this device
int rtl8139_init(rtl8139_state_t *state) {

    //Power on
    state->memar[CONFIG_1_REG] = 0;

    //Reset
    state->memar[CMD_REG] = CMD_RST_VAL;
    while(state->memar[CMD_REG] & CMD_RST_VAL)
        ;

    //Allocate physical memory for the network buffers
    if(state->rx_buffer == NULL && state->tx_buffer == NULL) {
        uintptr_t buffer_phys = pagealloc_alloc(0, 0, physmem_alloc_flags_data, RX_BUFFER_SIZE + TX_BUFFER_SIZE);
        intptr_t buffer_virt = vmem_phystovirt( (intptr_t)buffer_phys, RX_BUFFER_SIZE + TX_BUFFER_SIZE, vmem_flags_uncached | vmem_flags_kernel | vmem_flags_rw );

        state->rx_buffer_phys = buffer_phys;
        state->tx_buffer_phys = buffer_phys + RX_BUFFER_SIZE;

        state->rx_buffer = (uint8_t*)buffer_virt;
        state->tx_buffer = (uint8_t*)(buffer_virt + RX_BUFFER_SIZE);

        memset(state->rx_buffer, 0, RX_BUFFER_SIZE + TX_BUFFER_SIZE);

    } else if(!(state->rx_buffer != NULL && state->tx_buffer != NULL)) {
        DEBUG_PRINT("Unexpected driver state, likely memory corruption, init failed.\r\n");
        return -1;
    }

    if(state->rx_buffer_phys & ~0xffffffff) {
        DEBUG_PRINT("Unable to allocate 32-bit physical memory for this device!\r\n");
        return -1;
    }

    //Set the rx buffer
    *(uint32_t*)&state->memar[RBSTART_REG] = (uint32_t)state->rx_buffer_phys;

    //Configure interrupts
    *(uint16_t*)&state->memar[IMR_REG] = (INTR_ROK | INTR_TOK | INTR_TIMEOUT);

    //Configure receiver
    *(uint32_t*)&state->memar[RCR_REG] = RCR_RCV_PHYSMATCH | RCR_RCV_MULTICAST | RCR_RCV_BROADCAST | RCR_WRAP | RCR_RX_BUFLEN_64K;

    //Start the receiver and transmitter
    state->memar[CMD_REG] |= CMD_TX_EN | CMD_RX_EN;

    return 0;
}