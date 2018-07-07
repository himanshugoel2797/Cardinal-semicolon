// Copyright (c) 2018 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINAL_COREDISPLAY_DISPLAY_H
#define CARDINAL_COREDISPLAY_DISPLAY_H

#include <stdint.h>
#include <stdbool.h>

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

typedef enum {
    display_status_disconnected = 0,
    display_status_connected = 1,
} display_status_t;

typedef struct display_handlers {
    //set resolution
    int (*set_resolution) (void *state, display_res_info_t *info);
    
    //set brightness
    int (*set_brightness) (void *state, uint8_t brightness);

    //turn on/off
    int (*set_state)(void *state, bool on);

    //get framebuffer address
    int (*get_framebuffer)(void *state, uintptr_t *addr);

    //get connection status
    int (*get_status)(void *state, display_status_t *ans);

    //get display info
    int (*get_displayinfo)(void *state, display_res_info_t *res, int *entcnt);

    //Flush the framebuffer to the display
    int (*flush)(void *state);

} display_handlers_t;

typedef enum {
    display_features_autoresize = (1 << 0),
    display_features_requireflip = (1 << 1),
    display_features_hardware3d = (1 << 2),
} display_features_t;

typedef struct display_desc {
    char display_name[256];
    display_connection_t connection;
    display_handlers_t handlers;
    display_features_t features;
    void *state;
} display_desc_t;

int display_register(display_desc_t *desc);
int display_unregister(display_desc_t *desc);

#endif