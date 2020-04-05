/**
 * Copyright (c) 2020 Himanshu Goel
 * 
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

#include "stddef.h"
#include "stdint.h"

#include "devices.h"
#include "gmbus.h"

void igfx_gmbus_init(igfx_dev_state_t *driver_state)
{
    driver_state = NULL;
}

void igfx_gmbus_disable_writeprot(igfx_dev_state_t *driver)
{
    uint32_t val = igfx_read32(driver, driver->display_mmio_base + IGFX_GMBUS1);
    igfx_write32(driver, driver->display_mmio_base + IGFX_GMBUS1, val & ~(1 << 31));
}

void igfx_gmbus_enable_writeprot(igfx_dev_state_t *driver)
{
    uint32_t val = igfx_read32(driver, driver->display_mmio_base + IGFX_GMBUS1);
    igfx_write32(driver, driver->display_mmio_base + IGFX_GMBUS1, val | (1 << 31));
}

void igfx_gmbus_reset(igfx_dev_state_t *driver)
{
    igfx_write32(driver, driver->display_mmio_base + IGFX_GMBUS1, 0);
    igfx_gmbus_enable_writeprot(driver);
    igfx_gmbus_disable_writeprot(driver);
}

void igfx_gmbus_wait(igfx_dev_state_t *driver)
{
    while (!(igfx_read32(driver, driver->display_mmio_base + IGFX_GMBUS2) & (1 << 11)))
        ;
}

void igfx_gmbus_stoptransaction(igfx_dev_state_t *driver)
{
    igfx_write32(driver, driver->display_mmio_base + IGFX_GMBUS1, (1 << 30) | (1 << 27));
}

void igfx_gmbus_read(igfx_dev_state_t *driver, uint32_t disp_idx, uint8_t offset,
                     uint16_t sz, uint8_t *buf)
{
    uint32_t gmbus0_val = 0;
    uint32_t gmbus1_val = 0;

    if (driver->device->arch == IGFX_CHERRYTRAIL)
    {
        if (disp_idx == 0)
            gmbus0_val = 3;
    }

    gmbus1_val = (1 << 30) /*SW Ready*/ | (1 << 25) /*WAIT*/ |
                 (1 << 26) /*INDEX*/ | (sz & 511) << 16 | ((offset & 127) << 1) |
                 1 /*READ*/;
    igfx_gmbus_reset(driver);

    igfx_write32(driver, driver->display_mmio_base + IGFX_GMBUS0, gmbus0_val);
    igfx_write32(driver, driver->display_mmio_base + IGFX_GMBUS1, gmbus1_val);

    for (int i = 0; i < sz; i += 4)
    {
        igfx_gmbus_wait(driver);

        uint32_t bytes = igfx_read32(driver, driver->display_mmio_base + IGFX_GMBUS3);

        buf[i] = bytes & 0xFF;
        if (sz > i + 1)
            buf[i + 1] = (bytes >> 8) & 0xFF;
        if (sz > i + 2)
            buf[i + 2] = (bytes >> 16) & 0xFF;
        if (sz > i + 3)
            buf[i + 3] = (bytes >> 24) & 0xFF;
    }

    igfx_gmbus_stoptransaction(driver);

    print_hexdump(buf, sz);
}