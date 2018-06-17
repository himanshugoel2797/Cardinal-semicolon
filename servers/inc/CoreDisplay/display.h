// Copyright (c) 2018 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINAL_COREDISPLAY_DISPLAY_H
#define CARDINAL_COREDISPLAY_DISPLAY_H

#include <stdint.h>

typedef struct display_res_info {
    uint16_t w_res;
    uint16_t h_res;
    uint16_t stride;
    uint16_t refresh_rate;
} display_res_info_t;

typedef enum display_connection {
    display_connection_unkn,
    display_connection_hdmi,
    display_connection_DP,
} display_connection_t;

typedef struct display_info {
    uint16_t h_sz;
    uint16_t w_sz;
    uint16_t resolution_current;
    uint16_t resolutions_count;
    display_res_info_t *resolutions;
} display_info_t;

typedef struct display_handlers {
    //set resolution
    //set brightness
    //turn on/off
    //get framebuffer address
    //get connection status
    //get display info
} display_handlers_t;

typedef struct display_desc {
    display_connection_t connection;
    display_handlers_t handlers;
} display_desc_t;

#endif