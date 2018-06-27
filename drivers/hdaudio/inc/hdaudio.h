// Copyright (c) 2018 Himanshu Goel
// 
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINAL_DRIVERS_HDAUDIO_HDAUDIO_H
#define CARDINAL_DRIVERS_HDAUDIO_HDAUDIO_H

#include "regs.h"

typedef struct {
    uint32_t *buffer;
    int entcnt;
} hdaudio_buffer_def_t;

typedef struct hdaudio_instance{
    hdaudio_regs_t *cfg_regs;
    hdaudio_buffer_def_t corb;
    hdaudio_buffer_def_t rirb;

    struct hdaudio_instance *next;
} hdaudio_instance_t;

#endif