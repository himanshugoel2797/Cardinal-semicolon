// Copyright (c) 2018 Himanshu Goel
// 
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINAL_COREPOWER_POWER_H
#define CARDINAL_COREPOWER_POWER_H

#include <stdint.h>

typedef enum global_pwr_state {
    g0_pXX, //powered, specified performance state (0 = max performance)
    g1_s1, //power on suspend
    g1_s2, //cpu off
    g1_s3, //sleep
    g1_s4, //hibernate
    g2, //soft off - system off but power supply plugged in
} global_pwr_state_t;

typedef enum device_pwr_state {
    d0, //full on
    d1, //device defined
    d2, //device defined
    d3  //full off
} device_pwr_state_t;

typedef enum device_pwr_class {
    generic,
    display,
    audio_out,
    audio_in,
    human_interface_device,
    camera,
    processor,
} device_pwr_class_t;

//Power management event handlers
typedef struct pwr_device {
    char name[16];
    int (*event_g)(global_pwr_state_t tgt_state, int p_state);
    int (*event_d)(device_pwr_state_t tgt_state);
    int cur_pstate;
    device_pwr_state_t cur_dstate;
    global_pwr_state_t cur_gstate;
    device_pwr_class_t dev_class;
} pwr_device_t;

//Register a power management device
int pwr_register();

//Send a power state change event
int pwr_sendevent();

#endif