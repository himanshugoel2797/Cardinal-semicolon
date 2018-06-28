// Copyright (c) 2018 hgoel
// 
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINAL_DRIVERS_HDAUDIO_CMDS_H
#define CARDINAL_DRIVERS_HDAUDIO_CMDS_H

typedef enum {
    hdaudio_param_vendor_device_id = 0x0,
    hdaudio_param_revision_id = 0x2,
    hdaudio_param_node_cnt = 0x4,
    hdaudio_param_func_grp_type = 0x5,
    hdaudio_param_audio_grp_caps = 0x8,
    hdaudio_param_audio_widget_caps = 0x9,
    hdaudio_param_pcm_rate_caps = 0xa,
    hdaudio_param_fmt_caps = 0xb,
    hdaudio_param_pin_caps = 0xc,
    hdaudio_param_input_amp_caps = 0xd,
    hdaudio_param_output_amp_caps = 0x12,
    hdaudio_param_conn_list_len = 0xe,
    hdaudio_param_pwr_caps = 0xf,
    hdaudio_param_processing_caps = 0x10,
    hdaudio_param_gpio_cnt = 0x11,
    hdaudio_param_volume_caps = 0x13,
} hdaudio_param_type_t;

typedef enum {
    hdaudio_widget_audio_output,
    hdaudio_widget_audio_input,
    hdaudio_widget_audio_mixer,
    hdaudio_widget_audio_selector,
    hdaudio_widget_pin_complex,
    hdaudio_widget_power,
    hdaudio_widget_volume_knob,
} hdaudio_widget_type_t;

#define GET_PARAM(param_type) (0xF0000 | param_type)
#define GET_PARAMTYPE_FRM_PAYLOAD(payload) (payload & 0xFF)
#define GET_CONN_LIST(idx) (0xF0200 | idx) 
#define GET_CONN_LIST_OFF(payload) (payload & 0xFF)

#define GET_CFG_DEFAULT (0xF1C00) 

#endif