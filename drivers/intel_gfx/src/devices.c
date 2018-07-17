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

igfx_dev_t igfx_devices[] = {
    { "Intel Haswell-ULT Integrated Graphics", 0x0a16 },
    { NULL, 0 }
};

PRIVATE int igfx_getdevice(uint16_t devID, igfx_dev_t **dev) {
    int idx = 0;
    igfx_dev_t *iter = igfx_devices;
    while(iter->name != NULL) {
        if(iter->devID == devID) {
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

uint64_t igfx_read64(igfx_dev_state_t *dev, int off) {
    READ_N(uint64_t)
}

uint32_t igfx_read32(igfx_dev_state_t *dev, int off) {
    READ_N(uint32_t)
}

uint16_t igfx_read16(igfx_dev_state_t *dev, int off) {
    READ_N(uint16_t)
}

uint8_t igfx_read8(igfx_dev_state_t *dev, int off) {
    READ_N(uint8_t)
}

void igfx_write64(igfx_dev_state_t *dev, int off, uint64_t val) {
    WRITE_N(uint64_t)
}

void igfx_write32(igfx_dev_state_t *dev, int off, uint32_t val) {
    WRITE_N(uint32_t)
}

void igfx_write16(igfx_dev_state_t *dev, int off, uint16_t val) {
    WRITE_N(uint16_t)
}

void igfx_write8(igfx_dev_state_t *dev, int off, uint8_t val) {
    WRITE_N(uint8_t)
}