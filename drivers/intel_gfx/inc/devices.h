// Copyright (c) 2018 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINALSEMI_INTEL_GFX_DEVICES_H
#define CARDINALSEMI_INTEL_GFX_DEVICES_H

#include <stdint.h>

typedef struct {
    const char *name;
    uint16_t devID;
} igfx_dev_t;

typedef struct {
    igfx_dev_t *device;

    uintptr_t bar_phys;
    uint8_t *bar;
} igfx_dev_state_t;

extern igfx_dev_t igfx_devices[];

PRIVATE int igfx_getdevice(uint16_t devID, igfx_dev_t **dev);

uint64_t igfx_read64(igfx_dev_state_t *dev, int off);
uint32_t igfx_read32(igfx_dev_state_t *dev, int off);
uint16_t igfx_read16(igfx_dev_state_t *dev, int off);
uint8_t igfx_read8(igfx_dev_state_t *dev, int off);

void igfx_write64(igfx_dev_state_t *dev, int off, uint64_t val);
void igfx_write32(igfx_dev_state_t *dev, int off, uint32_t val);
void igfx_write16(igfx_dev_state_t *dev, int off, uint16_t val);
void igfx_write8(igfx_dev_state_t *dev, int off, uint8_t val);

#define igfx_setbits32(dev, off, flags) igfx_write32(dev, off, igfx_read32(dev, off) | flags)


#endif