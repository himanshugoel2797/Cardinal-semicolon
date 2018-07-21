/**
 * Copyright (c) 2018 Himanshu Goel
 * 
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

#include <stdint.h>
#include <stdbool.h>

#include "if_iwmreg.h"

#include "device_helpers.h"
#include "devices.h"

void iwifi_prepare(iwifi_dev_state_t *dev) {
    iwifi_setbits32(dev, IWM_CSR_HW_IF_CONFIG_REG, IWM_CSR_HW_IF_CONFIG_REG_PREPARE | IWM_CSR_HW_IF_CONFIG_REG_BIT_NIC_READY);

    //Wait for NIC ready
    while(true) {
        if(iwifi_read32(dev, IWM_CSR_HW_IF_CONFIG_REG) & IWM_CSR_HW_IF_CONFIG_REG_BIT_NIC_READY)
            break;
    }

    //Signal that the OS is ready
    iwifi_setbits32(dev, IWM_CSR_MBOX_SET_REG, IWM_CSR_MBOX_SET_REG_OS_ALIVE);
}

void iwifi_reset(iwifi_dev_state_t *dev_state) {
    iwifi_setbits32(dev_state, IWM_CSR_RESET, IWM_CSR_RESET_REG_FLAG_SW_RESET);

    //Wait for reset bit to read as being set
    while(~iwifi_read32(dev_state, IWM_CSR_RESET) & IWM_CSR_RESET_REG_FLAG_SW_RESET)
        ;
}

void iwifi_hw_start(iwifi_dev_state_t *state) {
    iwifi_reset(state);

    //Setup the HW kill interrupt
    iwifi_setbits32(state, IWM_CSR_INT_MASK, IWM_CSR_INT_BIT_RF_KILL);
    state->int_mask |= IWM_CSR_INT_BIT_RF_KILL;
}

bool iwifi_check_rfkill(iwifi_dev_state_t *state) {
    uint32_t gp = iwifi_read32(state, IWM_CSR_GP_CNTRL);
    return ~gp & IWM_CSR_GP_CNTRL_REG_FLAG_HW_RF_KILL_SW;
}

void iwifi_disable_interrupts(iwifi_dev_state_t *state) {
    state->int_mask = iwifi_read32(state, IWM_CSR_INT_MASK);
    iwifi_write32(state, IWM_CSR_INT_MASK, 0);
    iwifi_write32(state, IWM_CSR_INT, 0xffffffff);
}

void iwifi_clear_rfkillhandshake(iwifi_dev_state_t *state) {
    iwifi_write32(state, IWM_CSR_UCODE_DRV_GP1_CLR, IWM_CSR_UCODE_SW_BIT_RFKILL);
    iwifi_write32(state, IWM_CSR_UCODE_DRV_GP1_CLR, IWM_CSR_UCODE_DRV_GP1_BIT_CMD_BLOCKED);
}

void iwifi_rx_dma_state(iwifi_dev_state_t *state, bool enable) {
    if(enable) {
        uint32_t val =  IWM_FH_RCSR_RX_CONFIG_CHNL_EN_ENABLE_VAL |
	                    IWM_FH_RCSR_CHNL0_RX_IGNORE_RXF_EMPTY |
	                    IWM_FH_RCSR_CHNL0_RX_CONFIG_IRQ_DEST_INT_HOST_VAL |
	                    IWM_FH_RCSR_RX_CONFIG_REG_VAL_RB_SIZE_4K |
	                    (IWM_RX_RB_TIMEOUT << IWM_FH_RCSR_RX_CONFIG_REG_IRQ_RBTH_POS) |
	                    IWM_RX_QUEUE_SIZE_LOG << IWM_FH_RCSR_RX_CONFIG_RBDCB_SIZE_POS;

        iwifi_write32(state, IWM_FH_MEM_RCSR_CHNL0_CONFIG_REG, val);
    }else{
        iwifi_write32(state, IWM_FH_MEM_RCSR_CHNL0_CONFIG_REG, 0);

        while(true)
            if(iwifi_read32(state, IWM_FH_MEM_RSSR_RX_STATUS_REG) & IWM_FH_MEM_RSSR_RX_STATUS_REG)
                break;
    }
}