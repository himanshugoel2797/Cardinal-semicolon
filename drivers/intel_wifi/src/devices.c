/**
 * Copyright (c) 2018 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

#include <stdint.h>
#include <stdlib.h>
#include <types.h>
#include <mmio_bar.h>

#include "if_iwmreg.h"

#include "devices.h"

iwifi_dev_t iwifi_devices[] = {
    {"Intel Dual Band Wireless-AC 3168", "./iwifi_fw/3168.ucode", DEVID_3168_1, FAMILY_7000},
    {"Intel Dual Band Wireless-AC 3160", "./iwifi_fw/3160.ucode", DEVID_3160_1, FAMILY_7000},
    {"Intel Dual Band Wireless-AC 3160", "./iwifi_fw/3160.ucode", DEVID_3160_2, FAMILY_7000},
    {NULL, NULL, 0, 0}
};

PRIVATE int iwifi_getdevice(uint16_t devID, iwifi_dev_t **dev) {
    int idx = 0;
    iwifi_dev_t *iter = iwifi_devices;
    while(iter->name != NULL) {
        if(iter->devID == devID) {
            *dev = iter;
            return 0;
        }

        iter++;
    }
    return -1;
}

DEFINE_BAR_ACCESSORS(iwifi, iwifi_dev_state_t)

void iwifi_periph_write32(iwifi_dev_state_t *dev, int off, uint32_t val) {
    iwifi_write32(dev, IWM_HBUS_TARG_PRPH_WADDR, ((off & 0x000fffff) | (3 << 24)));
    iwifi_write32(dev, IWM_HBUS_TARG_PRPH_WDAT, val);
}

uint32_t iwifi_periph_read32(iwifi_dev_state_t *dev, int off) {
    iwifi_write32(dev, IWM_HBUS_TARG_PRPH_RADDR, ((off & 0x000fffff) | (3 << 24)));
    return iwifi_read32(dev, IWM_HBUS_TARG_PRPH_RDAT);
}