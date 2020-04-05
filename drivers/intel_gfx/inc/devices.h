// Copyright (c) 2018 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINALSEMI_INTEL_GFX_DEVICES_H
#define CARDINALSEMI_INTEL_GFX_DEVICES_H

#include <stdint.h>

#include "CoreDisplay/edid.h"

#define IGFX_CHERRYTRAIL 1
#define IGFX_HASWELL 2

#define IGFX_CHERRYTRAIL_DISP_BASE 0x180000
#define IGFX_HASWELL_DISP_BASE 0xC0000
#define IGFX_IRONLAKE_DISP_BASE 0xC0000

#define IGFX_CHERRYTRAIL_GTT_BASE 0x800000

typedef struct
{
    const char *name;
    uint16_t devID;
    uint8_t arch;
} igfx_dev_t;

typedef struct
{
    bool hotplug;
    bool connected;
    edid_t edid;
    uint32_t standard_mode;  //bit map that matches the edid standard timings
    int32_t custom_mode_idx; //index into the edid detailed modes, -1 for standard mode
} igfx_display_info_t;

typedef struct
{
    igfx_dev_t *device;

    uintptr_t bar_phys;
    uint8_t *bar;

    uint32_t display_count;
    igfx_display_info_t *displays;

    uint32_t display_mmio_base;
    uint32_t gtt_base;
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