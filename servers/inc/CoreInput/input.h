// Copyright (c) 2019 Himanshu Goel
// 
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINAL_COREINPUT_H
#define CARDINAL_COREINPUT_H

#include <stdint.h>

typedef enum {
    input_event_type_unknown,
    input_event_type_digital,
    input_event_type_analog,
} input_event_type_t;

typedef struct {
    uint32_t type : 2;
    uint32_t dev_id : 30;
    uint32_t timestamp;
    int32_t state;
} input_event_t;

#endif