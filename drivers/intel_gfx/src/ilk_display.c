/**
 * Copyright (c) 2026 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

// Ironlake (Gen5) display bring-up via firmware hand-off.
//
// This does NOT perform a cold mode-set (no FDI training / PCH DPLL /
// transcoder programming). It assumes the firmware has already lit the panel
// (the normal case after a UEFI/BIOS boot), reads the live plane/pipe state,
// exposes the existing framebuffer through the graphics aperture (BAR2), and
// registers it with CoreDisplay. A full cold mode-set is a future milestone.

#include "stddef.h"
#include "stdint.h"
#include <types.h>

#include "SysVirtualMemory/vmem.h"

#include "CoreDisplay/display.h"

#include "devices.h"
#include "ilk-regs.h"
#include "ilk_display.h"

#define IGFX_ILK_MAX_PLANES 2

static int igfx_ilk_getframebuffer(void *state, uintptr_t *addr)
{
    igfx_dev_state_t *driver = (igfx_dev_state_t *)state;
    *addr = driver->fb_virt;
    return 0;
}

static int igfx_ilk_getstatus(void *state, display_status_t *ans)
{
    (void)state;
    *ans = display_status_connected;
    return 0;
}

static int igfx_ilk_getdisplayinfo(void *state, display_res_info_t *res, int *entcnt)
{
    igfx_dev_state_t *driver = (igfx_dev_state_t *)state;
    *entcnt = 1;
    if (res != NULL)
    {
        res->w_res = (uint16_t)driver->fb_width;
        res->h_res = (uint16_t)driver->fb_height;
        res->stride = (uint16_t)driver->fb_stride;
        res->refresh_rate = 60;
    }
    return 0;
}

static int igfx_ilk_setbrightness(void *state, uint8_t brightness)
{
    igfx_dev_state_t *driver = (igfx_dev_state_t *)state;

    // The PWM period (max duty) lives in the upper 16 bits of PWM_MOD_FREQ.
    uint32_t max = (igfx_read32(driver, IGFX_ILK_PWM_MOD_FREQ) >> 16) & 0xFFFF;
    if (max == 0)
        return -1;

    uint32_t duty = ((uint32_t)brightness * max) / 255;
    uint32_t ctl1 = igfx_read32(driver, IGFX_ILK_BLC_PWM_CTL1) & 0xFFFF0000u;
    igfx_write32(driver, IGFX_ILK_BLC_PWM_CTL1, ctl1 | (duty & 0xFFFF));
    return 0;
}

static int igfx_ilk_setstate(void *state, bool on)
{
    igfx_dev_state_t *driver = (igfx_dev_state_t *)state;

    uint32_t plane_ctrl = igfx_read32(driver, IGFX_ILK_DSP_CTRL(driver->fb_plane));
    uint32_t pp_ctrl = igfx_read32(driver, IGFX_ILK_PP_CTRL);

    if (on)
    {
        igfx_write32(driver, IGFX_ILK_DSP_CTRL(driver->fb_plane),
                     plane_ctrl | IGFX_ILK_DSP_CTRL_ENABLE);
        igfx_write32(driver, IGFX_ILK_PP_CTRL, pp_ctrl | IGFX_ILK_PP_CTRL_BACKLIGHT);
    }
    else
    {
        igfx_write32(driver, IGFX_ILK_DSP_CTRL(driver->fb_plane),
                     plane_ctrl & ~IGFX_ILK_DSP_CTRL_ENABLE);
        igfx_write32(driver, IGFX_ILK_PP_CTRL, pp_ctrl & ~IGFX_ILK_PP_CTRL_BACKLIGHT);
    }
    return 0;
}

// The driver's state pointer is filled in at registration time (the device
// state is heap-allocated, so it cannot be a static initializer).
static display_desc_t igfx_ilk_display = {
    .display_name = "Intel Ironlake Display",
    .connection = display_connection_unkn,
    .handlers = {
        .set_resolution = NULL, // cold mode-set not implemented yet
        .set_brightness = igfx_ilk_setbrightness,
        .set_state = igfx_ilk_setstate,
        .get_framebuffer = igfx_ilk_getframebuffer,
        .get_status = igfx_ilk_getstatus,
        .get_displayinfo = igfx_ilk_getdisplayinfo,
        .flush = NULL, // direct scanout, no flip needed
    },
    .features = 0,
    .state = NULL,
};

void igfx_ilk_init(igfx_dev_state_t *driver)
{
    // Find the display plane the firmware left enabled.
    int plane = -1;
    for (int i = 0; i < IGFX_ILK_MAX_PLANES; i++)
    {
        if (igfx_read32(driver, IGFX_ILK_DSP_CTRL(i)) & IGFX_ILK_DSP_CTRL_ENABLE)
        {
            plane = i;
            break;
        }
    }

    if (plane < 0)
    {
        // Firmware did not light a plane; the hand-off path can't bring the
        // panel up on its own (cold mode-set is unimplemented).
        DEBUG_PRINT("[intel_gfx] Ironlake: no firmware-enabled display plane; skipping.\r\n");
        return;
    }

    // On Ironlake plane A drives pipe A and plane B drives pipe B.
    uint32_t pipe = (uint32_t)plane;

    uint32_t src = igfx_read32(driver, IGFX_ILK_PIPE_SRC(pipe));
    uint32_t width = ((src >> 16) & 0xFFF) + 1;
    uint32_t height = (src & 0xFFF) + 1;

    uint32_t stride = igfx_read32(driver, IGFX_ILK_DSP_STRIDE(plane));
    uint32_t surf = igfx_read32(driver, IGFX_ILK_DSP_SURF(plane)) & ~0xFFFu;

    if (driver->aperture_phys == 0 || width <= 1 || height <= 1 || stride == 0)
    {
        DEBUG_PRINT("[intel_gfx] Ironlake: invalid firmware framebuffer geometry; skipping.\r\n");
        return;
    }

    // Map the portion of the graphics aperture (BAR2) that backs the existing
    // framebuffer. Writes through the aperture go via the GTT to the pages the
    // firmware is already scanning out.
    size_t map_sz = (size_t)surf + (size_t)height * stride;
    map_sz = (map_sz + 0xFFF) & ~(size_t)0xFFF;

    uint8_t *aperture = (uint8_t *)vmem_phystovirt(
        (intptr_t)driver->aperture_phys, map_sz,
        vmem_flags_rw | vmem_flags_uncached | vmem_flags_kernel);

    driver->fb_plane = (uint32_t)plane;
    driver->fb_pipe = pipe;
    driver->fb_width = width;
    driver->fb_height = height;
    driver->fb_stride = stride;
    driver->fb_virt = (uintptr_t)aperture + surf;

    igfx_ilk_display.state = driver;
    if (display_register(&igfx_ilk_display) != 0)
    {
        DEBUG_PRINT("[intel_gfx] Ironlake: display_register failed.\r\n");
        return;
    }

    DEBUG_PRINT("[intel_gfx] Ironlake display registered (firmware hand-off).\r\n");
}
