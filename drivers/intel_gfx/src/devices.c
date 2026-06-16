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

#include "devices.h"

igfx_dev_t igfx_devices[] = {
    {"Intel Haswell-ULT Integrated Graphics", 0x0a16, IGFX_HASWELL},
    {"Intel Cherrylake-E8000/J3xxx/N3xxx Graphics", 0x22b0, IGFX_CHERRYTRAIL},
    {"Intel Ironlake Mobile (Arrandale) Graphics", 0x0046, IGFX_IRONLAKE},
    {"Intel Ironlake Desktop (Clarkdale) Graphics", 0x0042, IGFX_IRONLAKE},
    {NULL, 0, 0}};

PRIVATE int igfx_getdevice(uint16_t devID, igfx_dev_t **dev)
{
    int idx = 0;
    igfx_dev_t *iter = igfx_devices;
    while (iter->name != NULL)
    {
        if (iter->devID == devID)
        {
            *dev = iter;
            return 0;
        }

        iter++;
    }
    return -1;
}

DEFINE_BAR_ACCESSORS(igfx, igfx_dev_state_t)