/**
 * Copyright (c) 2020 Himanshu Goel
 * 
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */
#include "stddef.h"
#include "stdint.h"
#include "stdlib.h"

#include "devices.h"

#include "display.h"
#include "gmbus.h"

void igfx_display_init(igfx_dev_state_t *driver)
{
    // determine port count for the specific architecture
    // detect currently attached ports and set them up
    if (driver->device->arch == IGFX_CHERRYTRAIL)
    {
        // 2 pluggable ports
        // 1 internal
        // only support one external port for now
        driver->display_count = 1;
        driver->displays = malloc(sizeof(igfx_display_info_t));
        driver->displays[0].hotplug = true;
        driver->displays[0].connected = false;

        igfx_write32(driver,
                     driver->display_mmio_base + IGFX_CHV_PORT_HOTPLUG_DETECT,
                     IGFX_CHV_PORT_HOTPLUG_STAT_BIT_HDMI_D); // enable hot plug
                                                             // detection for HDMI D

        igfx_display_checkhotplugs(driver);
    }
}

bool igfx_display_isconnected(igfx_dev_state_t *driver, uint32_t idx)
{
    if (driver->device->arch == IGFX_CHERRYTRAIL && driver->display_count > idx)
    {
        if (idx == 0)
            return (igfx_read32(driver, driver->display_mmio_base +
                                            IGFX_CHV_PORT_HOTPLUG_STAT) &
                    IGFX_CHV_PORT_HOTPLUG_STAT_BIT_HDMI_D) != 0;
    }

    return false;
}

void igfx_display_enable_port(igfx_dev_state_t *driver, uint32_t idx)
{
    if (driver->device->arch == IGFX_CHERRYTRAIL)
    {
        if (idx == 0)
        {
            // TODO ensure port is HDMI by checking the EDID
            // Enable HDMI port D
            // Obtain the vsync and hsync polarities from the EDID
            bool v_pol =
                driver->displays[idx]
                    .edid.detailed_modes[driver->displays[idx].custom_mode_idx]
                    .v_polarity;
            bool h_pol =
                driver->displays[idx]
                    .edid.detailed_modes[driver->displays[idx].custom_mode_idx]
                    .h_polarity;

            //Specify TMDS encoding (2 << 10)
            //

            uint32_t hdmid = igfx_read32(driver, driver->display_mmio_base + IGFX_CHV_HDMID);
            hdmid &= IGFX_CHV_HDMID_MASK;
            hdmid |= IGFX_CHV_HDMID_ENABLE /*| IGFX_CHV_HDMID_AUDIO_OUT*/ |
                     IGFX_CHV_HDMID_TMDS_ENC |
                     IGFX_CHV_HDMID_NULL_PACKETS | (2 << 10) /*ENCODING*/ |
                     (v_pol << 4) | (h_pol << 3);
            igfx_write32(driver, driver->display_mmio_base + IGFX_CHV_HDMID, hdmid);
        }
    }
}

void igfx_display_pipe_setup(igfx_dev_state_t *driver, uint32_t idx)
{
    if (driver->device->arch ==
        IGFX_CHERRYTRAIL)
    { // port D only connects to PIPE C
        if (idx == 0)
        { // Port D is only connected to PIPE C
            // Setup PIPE C
            // ignore pp status bits because this is an external port
            // disable pipc_pp_control

            // Pipe C config:
            // dpllc_ctrl <- not needed?
            // dpllcmd <- not needed?
            // video_dip_ctl_c <- only enable? - additional dip frames are optional,
            // set port to 0x3
            igfx_write32(driver, driver->display_mmio_base + IGFX_CHV_VIDEO_DIP_CTL_C,
                         igfx_read32(driver, driver->display_mmio_base +
                                                 IGFX_CHV_VIDEO_DIP_CTL_C) |
                             IGFX_CHV_VIDEO_DIP_CTL_ENABLE_DIP |
                             IGFX_CHV_VIDEO_DIP_CTL_PORT_SEL);
            // video_dip_data_c <- not needed? - understand what this
            // is pfit_control    <- panel fitter, shouldn't be necessary

            uint32_t hactive =
                driver->displays[idx]
                    .edid.detailed_modes[driver->displays[idx].custom_mode_idx]
                    .hactive;
            uint32_t hblank =
                driver->displays[idx]
                    .edid.detailed_modes[driver->displays[idx].custom_mode_idx]
                    .hblank;
            uint32_t hborder =
                driver->displays[idx]
                    .edid.detailed_modes[driver->displays[idx].custom_mode_idx]
                    .hborder;
            uint32_t hporch =
                driver->displays[idx]
                    .edid.detailed_modes[driver->displays[idx].custom_mode_idx]
                    .hsync_porch;
            uint32_t hpulse =
                driver->displays[idx]
                    .edid.detailed_modes[driver->displays[idx].custom_mode_idx]
                    .hsync_pulse;

            uint32_t vactive =
                driver->displays[idx]
                    .edid.detailed_modes[driver->displays[idx].custom_mode_idx]
                    .vactive;
            uint32_t vblank =
                driver->displays[idx]
                    .edid.detailed_modes[driver->displays[idx].custom_mode_idx]
                    .vblank;
            uint32_t vborder =
                driver->displays[idx]
                    .edid.detailed_modes[driver->displays[idx].custom_mode_idx]
                    .vborder;
            uint32_t vporch =
                driver->displays[idx]
                    .edid.detailed_modes[driver->displays[idx].custom_mode_idx]
                    .vsync_porch;
            uint32_t vpulse =
                driver->displays[idx]
                    .edid.detailed_modes[driver->displays[idx].custom_mode_idx]
                    .vsync_pulse;

            uint32_t htotal = hactive + hblank + hborder;
            uint32_t hblank_end = hborder + hactive + hblank;
            uint32_t hblank_start = hborder + hactive;
            uint32_t hsync_end = hactive + hborder + hporch + hpulse;
            uint32_t hsync_start = hactive + hborder + hporch;

            uint32_t vtotal = vactive + vblank + vborder;
            uint32_t vblank_end = vborder + vactive + vblank;
            uint32_t vblank_start = vborder + vactive;
            uint32_t vsync_end = vactive + vborder + vporch + vpulse;
            uint32_t vsync_start = vactive + vborder + vporch;

            // htotal <-
            // 28:16 active + blank + border - 1, must be multiple of 2
            // 11:0 active, active - 1
            igfx_write32(driver, driver->display_mmio_base + IGFX_CHV_HTOTAL,
                         ((htotal - 1) & 0x1FFF) << 16 | ((hactive - 1) & 0xFFF));

            // hblank <-
            // 28:16 border + active + blank - 1
            // 12:0 border + active - 1
            igfx_write32(driver, driver->display_mmio_base + IGFX_CHV_HBLANK,
                         ((hblank_end - 1) & 0x1FFF) << 16 |
                             ((hblank_start - 1) & 0x1FFF));

            // hsync
            // 28:16 active + border + porch + sync - 1
            // 12:0 active + border + porch - 1
            igfx_write32(driver, driver->display_mmio_base + IGFX_CHV_HSYNC,
                         ((hsync_end - 1) & 0x1FFF) << 16 |
                             ((hsync_start - 1) & 0x1FFF));

            // vtotal
            // 28:16 active + border + blank - 1
            // 11:0 active - 1
            igfx_write32(driver, driver->display_mmio_base + IGFX_CHV_VTOTAL,
                         ((vtotal - 1) & 0x1FFF) << 16 | ((vactive - 1) & 0xFFF));

            // vblank
            // 28:16 active + border + blank - 1
            // 12:0 active + border - 1
            igfx_write32(driver, driver->display_mmio_base + IGFX_CHV_VBLANK,
                         ((vblank_end - 1) & 0x1FFF) << 16 |
                             ((vblank_start - 1) & 0x1FFF));

            // vsync
            // 28:16 active + border + porch + sync - 1
            // 12:0 active + border + porch - 1
            igfx_write32(driver, driver->display_mmio_base + IGFX_CHV_VSYNC,
                         ((vsync_end - 1) & 0x1FFF) << 16 |
                             ((vsync_start - 1) & 0x1FFF));

            // pipecsrc
            // 27:16 h_res
            // 11:0 v_res
            igfx_write32(driver, driver->display_mmio_base + IGFX_CHV_PIPECSRC,
                         ((hactive - 1) & 0xFFF) << 16 | ((vactive - 1) & 0xFFF));

            // pipec_dsl <- ro
            // pipec_slc <- not needed?
            // pipecconf
            // 31: enable
            igfx_write32(driver, driver->display_mmio_base + IGFX_CHV_PIPECCONF,
                         (1 << 31));

            // pipecstat <- needed for interrupts
            // pipecframecount <- not really needed
            // pipecflipcount <- not really needed
            // pipecmsamisc <- stereo output related, not needed

            // fw1 <- check
            // fw2 <- check

            //GTT setup - replace the current blocks with custom ones
            //check if other things need the full memory map

            // Plane C config:
            // dspcaddr <- offset from graphics memory aperture base, mapped through
            // GTT (async flip)
            // dspclinoffset <- not needed
            // dspcstride
            // 31:6: stride in bytes
            // dspckeyval <- not needed, aren't using plane keying
            // dspcsurf <- offset from graphics memory aperture base, mapped through
            // GTT
            // dspcntr <-
            // 31: enable 30: gamma enable - needs gamma config in pipecconf
            // 29:26 pixel format

            // note, for audio:
            // aud_config_c
            // aud_misc_ctrl_c
            // aud_cts_enable_c
            //
        }
    }
}

void igfx_display_checkhotplugs(igfx_dev_state_t *driver)
{
    for (uint32_t i = 0; i < driver->display_count; i++)
    {
        bool disp_connected = igfx_display_isconnected(driver, i);
        if (disp_connected &&
            !driver->displays[i].connected)
        { // display was newly connected
            // read the EDID
            uint8_t edid_raw[0x80];
            igfx_gmbus_read(driver, i, 0x50, 0x80, edid_raw);
            if (!coredisplay_parse_edid(edid_raw, &driver->displays[i].edid))
            {
                print_str("Failed to parse EDID!\r\n");
                continue;
            }
            // choose the preferred timings
            driver->displays[i].custom_mode_idx = 0;
            driver->displays[i].standard_mode = 0;
            driver->displays[i].connected = true;

            // configure a pipe to the display
            igfx_display_enable_port(driver, i);
            igfx_display_pipe_setup(driver, i);
        }
        else if (!disp_connected &&
                 driver->displays[i].connected)
        { // display just disconnected
            // disable the associated pipe
            driver->displays[i].connected = false;
        }
    }
}