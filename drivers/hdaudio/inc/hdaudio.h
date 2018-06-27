// Copyright (c) 2018 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINAL_DRIVERS_HDAUDIO_HDAUDIO_H
#define CARDINAL_DRIVERS_HDAUDIO_HDAUDIO_H

#include "regs.h"

typedef struct {
    uint32_t vendor_dev_id;
    uint8_t starting_sub_node;
    uint8_t sub_node_cnt;
    uint8_t grp_type;

    uint8_t input_delay;
    uint8_t output_delay;

    uint32_t caps;

    uint32_t pcm_rates;
    uint32_t stream_formats;
    uint32_t pin_cap;
    uint32_t input_amp_cap;
    uint32_t output_amp_cap;
    uint32_t conn_list_len;
    uint32_t pwr_states;
    uint32_t process_caps;
    uint32_t gpio_count;
    uint32_t volume_caps;

    uint8_t *conn_list;
} hdaudio_node_t;

typedef struct {
    uint32_t *buffer;
    uintptr_t buffer_phys;
    int entcnt;
} hdaudio_buffer_def_t;

typedef struct hdaudio_cmd_entry hdaudio_cmd_entry_t;

typedef struct hdaudio_instance {
    hdaudio_regs_t *cfg_regs;
    hdaudio_buffer_def_t corb;
    hdaudio_buffer_def_t rirb;
    hdaudio_cmd_entry_t *cmds;

    int interrupt_vec;
    uint32_t codecs;

    hdaudio_node_t **nodes;

    struct hdaudio_instance *next;
} hdaudio_instance_t;

struct hdaudio_cmd_entry {
    bool waiting;
    bool handled;

    uint8_t addr;
    uint8_t node;
    uint32_t payload;

    void (*handler)(hdaudio_instance_t *instance, hdaudio_cmd_entry_t *ent, uint64_t val);
};

#endif