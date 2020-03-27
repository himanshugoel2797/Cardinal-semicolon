// Copyright (c) 2019 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINAL_COREINPUT_H
#define CARDINAL_COREINPUT_H

#include <stdint.h>
#include <stdbool.h>

typedef struct
{
    uint64_t timestamp;
    bool is_btn_event;
    uint32_t index;
    union {
        bool state;
        float position;
    };
} input_device_event_t;

typedef enum
{
    input_device_type_unknown = 0,
    input_device_type_mouse = 1,
    input_device_type_keyboard = 2,
    input_device_type_controller = 3,
    input_device_type_joystick = 4,
    input_device_type_throttle = 5,
    input_device_type_touchpad = 6,
    input_device_type_touchscreen = 7,
} input_device_type_t;

typedef enum
{
    input_device_features_none = 0,
    input_device_features_absolutepos = (1 << 0),
} input_device_features_t;

typedef struct input_device_handlers
{
    bool (*has_pending)(void *state);
    void (*read)(void *state, input_device_event_t *events);
} input_device_handlers_t;

typedef struct
{
    char name[256];
    input_device_features_t features;
    input_device_handlers_t handlers;
    input_device_type_t type;
    void *state;
} input_device_desc_t;

int input_device_register(input_device_desc_t *desc);
int input_device_unregister(input_device_desc_t *desc);

typedef enum
{
    kbd_keys_unkn = 0,
    kbd_keys_A,
    kbd_keys_B,
    kbd_keys_C,
    kbd_keys_D,
    kbd_keys_E,
    kbd_keys_F,
    kbd_keys_G,
    kbd_keys_H,
    kbd_keys_I,
    kbd_keys_J,
    kbd_keys_K,
    kbd_keys_L,
    kbd_keys_M,
    kbd_keys_N,
    kbd_keys_O,
    kbd_keys_P,
    kbd_keys_Q,
    kbd_keys_R,
    kbd_keys_S,
    kbd_keys_T,
    kbd_keys_U,
    kbd_keys_V,
    kbd_keys_W,
    kbd_keys_X,
    kbd_keys_Y,
    kbd_keys_Z,
    kbd_keys_0,
    kbd_keys_1,
    kbd_keys_2,
    kbd_keys_3,
    kbd_keys_4,
    kbd_keys_5,
    kbd_keys_6,
    kbd_keys_7,
    kbd_keys_8,
    kbd_keys_9,
    kbd_keys_tick,
    kbd_keys_sub,
    kbd_keys_eq,
    kbd_keys_bckslash,
    kbd_keys_bksp,
    kbd_keys_space,
    kbd_keys_tab,
    kbd_keys_caps,
    kbd_keys_lshft,
    kbd_keys_lctrl,
    kbd_keys_lwin,
    kbd_keys_lalt,
    kbd_keys_rshft,
    kbd_keys_rctrl,
    kbd_keys_rwin,
    kbd_keys_ralt,
    kbd_keys_apps,
    kbd_keys_enter,
    kbd_keys_esc,
    kbd_keys_f1,
    kbd_keys_f2,
    kbd_keys_f3,
    kbd_keys_f4,
    kbd_keys_f5,
    kbd_keys_f6,
    kbd_keys_f7,
    kbd_keys_f8,
    kbd_keys_f9,
    kbd_keys_f10,
    kbd_keys_f11,
    kbd_keys_f12,
    kbd_keys_prntscrn,
    kbd_keys_scroll,
    kbd_keys_pause,
    kbd_keys_lsqbrac,
    kbd_keys_insert,
    kbd_keys_home,
    kbd_keys_pgup,
    kbd_keys_del,
    kbd_keys_end,
    kbd_keys_pgdn,
    kbd_keys_up,
    kbd_keys_left,
    kbd_keys_down,
    kbd_keys_right,
    kbd_keys_num,
    kbd_keys_kp_div,
    kbd_keys_kp_mul,
    kbd_keys_kp_sub,
    kbd_keys_kp_add,
    kbd_keys_kp_en,
    kbd_keys_kp_dot,
    kbd_keys_kp_0,
    kbd_keys_kp_1,
    kbd_keys_kp_2,
    kbd_keys_kp_3,
    kbd_keys_kp_4,
    kbd_keys_kp_5,
    kbd_keys_kp_6,
    kbd_keys_kp_7,
    kbd_keys_kp_8,
    kbd_keys_kp_9,
    kbd_keys_rsqbrac,
    kbd_keys_semicolon,
    kbd_keys_apostrophe,
    kbd_keys_comma,
    kbd_keys_period,
    kbd_keys_fwdslash,
} kbd_keys_t;

#endif