/**
 * Copyright (c) 2018 Himanshu Goel
 * 
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

#include <stdint.h>
#include <stdlib.h>
#include <types.h>

#include "devices.h"

iwifi_dev_t iwifi_devices[] = {
    {"Intel Dual Band Wireless-AC 3168", "./iwifi_fw/3168.ucode", DEVID_3168_1, FAMILY_7000},
    {NULL, NULL, 0, 0}
};

PRIVATE int iwifi_getdevice(uint16_t devID, iwifi_dev_t **dev) {
    int idx = 0;
    iwifi_dev_t *iter = iwifi_devices;
    while(iter->name != NULL) {
        if(iter->devID == devID){
            *dev = iter;
            return 0;
        }

        iter++;
    }
    return -1;
}

#define READ_N(type)                                        \
    if(off % sizeof( type ) != 0)                           \
        PANIC("Invalid offset");                            \
    return ((type *)dev->bar)[off / sizeof( type )];        \

#define WRITE_N(type)                                       \
    if(off % sizeof( type ) != 0)                           \
        PANIC("Invalid offset");                            \
    ((type *)dev->bar)[off / sizeof( type )] = val;         \

uint64_t iwifi_read64(iwifi_dev_state_t *dev, int off) {
    READ_N(uint64_t)
}

uint32_t iwifi_read32(iwifi_dev_state_t *dev, int off) {
    READ_N(uint32_t)
}

uint16_t iwifi_read16(iwifi_dev_state_t *dev, int off) {
    READ_N(uint16_t)
}

uint8_t iwifi_read8(iwifi_dev_state_t *dev, int off) {
    READ_N(uint8_t)
}

void iwifi_write64(iwifi_dev_state_t *dev, int off, uint64_t val) {
    WRITE_N(uint64_t)
}

void iwifi_write32(iwifi_dev_state_t *dev, int off, uint32_t val) {
    WRITE_N(uint32_t)
}

void iwifi_write16(iwifi_dev_state_t *dev, int off, uint16_t val) {
    WRITE_N(uint16_t)
}

void iwifi_write8(iwifi_dev_state_t *dev, int off, uint8_t val) {
    WRITE_N(uint8_t)
}