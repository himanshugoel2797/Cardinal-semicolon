/**
 * Copyright (c) 2018 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

#include <stdint.h>

#include "iwl-csr.h"

#include "devices.h"
#include "pwm.h"

int iwifi_notify_ready(iwifi_dev_state_t *dev) {

    iwifi_setbits32(dev, IWM_CSR_HW_IF_CONFIG_REG, IWM_CSR_HW_IF_CONFIG_REG_PREPARE | IWM_CSR_HW_IF_CONFIG_REG_BIT_NIC_READY);

    //Wait for NIC ready
    while(true) {
        if(iwifi_read32(dev, IWM_CSR_HW_IF_CONFIG_REG) & IWM_CSR_HW_IF_CONFIG_REG_BIT_NIC_READY)
            break;
    }

    //Signal that the OS is ready
    iwifi_setbits32(dev, IWM_CSR_MBOX_SET_REG, IWM_CSR_MBOX_SET_REG_OS_ALIVE);

    return 0;
}