// Copyright (c) 2020 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINALSEMI_IGFX_DISPLAY_H
#define CARDINALSEMI_IGFX_DISPLAY_H

#include "stdint.h"
#include "devices.h"

#define IGFX_CHV_PORT_HOTPLUG_DETECT (0x61110)

#define IGFX_CHV_PORT_HOTPLUG_STAT (0x61114)
#define IGFX_CHV_PORT_HOTPLUG_STAT_BIT_HDMI_D (1 << 27)

#define IGFX_CHV_HDMID (0x6116C)
#define IGFX_CHV_HDMID_DDI_PORT_DET (1 << 1)
#define IGFX_CHV_HDMID_DIG_PORT_DET (1 << 2)
#define IGFX_CHV_HDMID_SYNC_POLARITY (3 << 3)
#define IGFX_CHV_HDMID_AUDIO_OUT (1 << 6)
#define IGFX_CHV_HDMID_COLOR_RANGE (1 << 8)
#define IGFX_CHV_HDMID_NULL_PACKETS (1 << 9)
#define IGFX_CHV_HDMID_ENCODING (3 << 10)
#define IGFX_CHV_HDMID_PIPE_SELECT (1 << 30)
#define IGFX_CHV_HDMID_ENABLE (1 << 31)
#define IGFX_CHV_HDMID_TMDS_ENC (2 << 10)
#define IGFX_CHV_HDMID_MASK ((127 << 19) | (1 << 29) | (1 << 15) | (1 << 14) | (1 << 13) | (1 << 12))

#define IGFX_CHV_VIDEO_DIP_CTL_C (0x611F0)
#define IGFX_CHV_VIDEO_DIP_CTL_PORT_SEL (3 << 29)
#define IGFX_CHV_VIDEO_DIP_CTL_ENABLE_DIP (1 << 31)

#define IGFX_CHV_HTOTAL (0x63000)
#define IGFX_CHV_HBLANK (0x63004)
#define IGFX_CHV_HSYNC (0x63008)
#define IGFX_CHV_VTOTAL (0x6300C)
#define IGFX_CHV_VBLANK (0x63010)
#define IGFX_CHV_VSYNC (0x63014)
#define IGFX_CHV_PIPECSRC (0x6301C)
#define IGFX_CHV_PIPECCONF (0x74008)

#define IGFX_CHV_DSPCADDR (0x7417C)
#define IGFX_CHV_DSPCNTR (0x74180)
#define IGFX_CHV_DSPCSTRIDE (0x74188)
#define IGFX_CHV_DSPCSURF (0x7419C)

void igfx_display_init(igfx_dev_state_t *dev);
void igfx_display_checkhotplugs(igfx_dev_state_t *dev);

#endif