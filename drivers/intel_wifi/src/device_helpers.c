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
    iwifi_setbits32(dev, IWM_CSR_HW_IF_CONFIG_REG, IWM_CSR_HW_IF_CONFIG_REG_PREPARE);
    iwifi_setbits32(dev, IWM_CSR_HW_IF_CONFIG_REG, IWM_CSR_HW_IF_CONFIG_REG_BIT_NIC_READY);

    //Wait for NIC ready
    while(true) {
        DEBUG_PRINT("NIC Ready Wait\r\n");
        if(iwifi_read32(dev, IWM_CSR_HW_IF_CONFIG_REG) & IWM_CSR_HW_IF_CONFIG_REG_BIT_NIC_READY)
            break;
    }

    //Signal that the OS is ready
    iwifi_setbits32(dev, IWM_CSR_MBOX_SET_REG, IWM_CSR_MBOX_SET_REG_OS_ALIVE);
}

void iwifi_reset(iwifi_dev_state_t *dev_state) {
    iwifi_setbits32(dev_state, IWM_CSR_RESET, IWM_CSR_RESET_REG_FLAG_SW_RESET);

    //Wait for reset bit to read as being set
    while(iwifi_read32(dev_state, IWM_CSR_RESET) & IWM_CSR_RESET_REG_FLAG_SW_RESET)
        DEBUG_PRINT("Reset Wait\r\n");
}

void iwifi_hw_start(iwifi_dev_state_t *state) {
    iwifi_reset(state);

    //configure power management
    iwifi_setbits32(state, IWM_CSR_GIO_CHICKEN_BITS, IWM_CSR_GIO_CHICKEN_BITS_REG_BIT_DIS_L0S_EXIT_TIMER);
    iwifi_setbits32(state, IWM_CSR_GIO_CHICKEN_BITS, IWM_CSR_GIO_CHICKEN_BITS_REG_BIT_L1A_NO_L0S_RX);
	iwifi_setbits32(state, IWM_CSR_DBG_HPET_MEM_REG, IWM_CSR_DBG_HPET_MEM_REG_VAL);
    iwifi_setbits32(state, IWM_CSR_HW_IF_CONFIG_REG, IWM_CSR_HW_IF_CONFIG_REG_BIT_HAP_WAKE_L1A);
    iwifi_clrbits32(state, IWM_CSR_GIO_REG, IWM_CSR_GIO_REG_VAL_L0S_ENABLED);

    iwifi_setbits32(state, IWM_CSR_GP_CNTRL, IWM_CSR_GP_CNTRL_REG_FLAG_INIT_DONE);
    while(true) {
        DEBUG_PRINT("MAC Ready Wait\r\n");
        if(iwifi_read32(state, IWM_CSR_GP_CNTRL) & IWM_CSR_GP_CNTRL_REG_FLAG_MAC_CLOCK_READY)
            break;
    }

    iwifi_lock(state);
    iwifi_periph_read32(state, IWM_OSC_CLK);
	iwifi_periph_read32(state, IWM_OSC_CLK);
	iwifi_periph_setbits32(state, IWM_OSC_CLK, IWM_OSC_CLK_FORCE_CONTROL);
	iwifi_periph_read32(state, IWM_OSC_CLK);
	iwifi_periph_read32(state, IWM_OSC_CLK);
	iwifi_unlock(state);
	
    iwifi_lock(state);
    iwifi_periph_write32(state, IWM_APMG_CLK_EN_REG, IWM_APMG_CLK_VAL_DMA_CLK_RQT);
    iwifi_periph_setbits32(state, IWM_APMG_PCIDEV_STT_REG, IWM_APMG_PCIDEV_STT_VAL_L1_ACT_DIS);
    iwifi_periph_write32(state, IWM_APMG_RTC_INT_STT_REG, IWM_APMG_RTC_INT_STT_RFKILL);
    
    uint32_t reg = iwifi_periph_read32(state, IWM_APMG_PS_CTRL_REG);
    reg = reg & ~IWM_APMG_PS_CTRL_MSK_PWR_SRC;
    iwifi_periph_write32(state, IWM_APMG_PS_CTRL_REG, reg);
    
    iwifi_unlock(state);

    //Setup the HW kill interrupt
    //iwifi_setbits32(state, IWM_CSR_INT_MASK, IWM_CSR_INT_BIT_RF_KILL);
    //state->int_mask |= IWM_CSR_INT_BIT_RF_KILL;
}

bool iwifi_check_rfkill(iwifi_dev_state_t *state) {
    uint32_t gp = iwifi_read32(state, IWM_CSR_GP_CNTRL);
    state->rf_kill = !(gp & IWM_CSR_GP_CNTRL_REG_FLAG_HW_RF_KILL_SW);

    return state->rf_kill;
}

void iwifi_disable_interrupts(iwifi_dev_state_t *state) {
    state->int_mask = iwifi_read32(state, IWM_CSR_INT_MASK);
    iwifi_write32(state, IWM_CSR_INT_MASK, 0);
    iwifi_write32(state, IWM_CSR_INT, 0xffffffff);
    iwifi_write32(state, IWM_CSR_FH_INT_STATUS, 0xffffffff);
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
	    
        iwifi_write8(state, IWM_CSR_INT_COALESCING, IWM_HOST_INT_TIMEOUT_DEF);
        iwifi_setbits32(state, IWM_CSR_INT_COALESCING, IWM_HOST_INT_OPER_MODE);

	    iwifi_write32(state, IWM_FH_RSCSR_CHNL0_WPTR, 8);
    }else{
        iwifi_write32(state, IWM_FH_MEM_RCSR_CHNL0_CONFIG_REG, 0);

        while(true){
            DEBUG_PRINT("RX Wait\r\n");
            if(iwifi_read32(state, IWM_FH_MEM_RSSR_RX_STATUS_REG) & IWM_FH_RSSR_CHNL0_RX_STATUS_CHNL_IDLE)
                break;
        }
    }
}

void iwifi_tx_sched_dma_state(iwifi_dev_state_t *state, bool enable){
    if(enable) {
	    iwifi_periph_write32(state, IWM_SCD_GP_CTRL, IWM_SCD_GP_CTRL_AUTO_ACTIVE_MODE);
    }else{
        iwifi_periph_write32(state, IWM_SCD_TXFACT, 0);
    }
}

void iwifi_lock(iwifi_dev_state_t *state) {
    uint32_t cmp_val = IWM_CSR_GP_CNTRL_REG_FLAG_MAC_CLOCK_READY | IWM_CSR_GP_CNTRL_REG_FLAG_GOING_TO_SLEEP;

    iwifi_setbits32(state, IWM_CSR_GP_CNTRL, IWM_CSR_GP_CNTRL_REG_FLAG_MAC_ACCESS_REQ);

    while(true) {
        DEBUG_PRINT("Lock Wait\r\n");
        if(iwifi_read32(state, IWM_CSR_GP_CNTRL) & 0x10)
            DEBUG_PRINT("Clock sleeping\r\n");

        if((iwifi_read32(state, IWM_CSR_GP_CNTRL) & cmp_val) == IWM_CSR_GP_CNTRL_REG_FLAG_MAC_CLOCK_READY)
            break;
    }
}

void iwifi_unlock(iwifi_dev_state_t *state) {
    iwifi_clrbits32(state, IWM_CSR_GP_CNTRL, IWM_CSR_GP_CNTRL_REG_FLAG_MAC_ACCESS_REQ);
}