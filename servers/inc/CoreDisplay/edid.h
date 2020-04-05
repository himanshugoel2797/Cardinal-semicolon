// Copyright (c) 2020 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT
#ifndef CARDINALSEMI_COREDISP_COMMON_EDID_H
#define CARDINALSEMI_COREDISP_COMMON_EDID_H

#include "stdbool.h"
#include "stdint.h"

typedef enum
{
    edid_port_type_undef = 0,
    edid_port_type_undef2,
    edid_port_type_hdmia,
    edid_port_type_hdmib,
    edid_port_type_mddi,
    edid_port_type_displayport,
} edid_port_type_t;

typedef enum
{
    edid_established_modes_none = 0,
} edid_established_modes_t;

typedef struct
{
    uint8_t v_polarity;
    uint8_t h_polarity;
    uint8_t hborder;
    uint8_t vborder;
    uint32_t hactive;
    uint32_t vactive;
    uint32_t hblank;
    uint32_t vblank;
    uint32_t hsync_porch;
    uint32_t vsync_porch;
    uint32_t hsync_pulse;
    uint32_t vsync_pulse;
    uint16_t pixel_clock;
    uint16_t hsize_mm;
    uint16_t vsize_mm;
} edid_detailed_mode_t;

typedef struct
{
    uint16_t h_res;
    uint16_t v_res;
    uint16_t v_freq;
    uint8_t aspect_num;
    uint8_t aspect_denom;
} edid_standard_timings_t;

typedef struct
{
    edid_port_type_t port_type;
    uint8_t bit_depth;
    uint8_t gamma; // divide by 100 and add 1
    edid_established_modes_t established_modes;
    uint8_t standard_timing_count;
    edid_standard_timings_t standard_timings[8];
    char display_name[13];
    uint8_t detailed_mode_count;
    edid_detailed_mode_t detailed_modes[4];

} edid_t;

bool coredisplay_parse_edid(uint8_t *raw, edid_t *result);

#endif