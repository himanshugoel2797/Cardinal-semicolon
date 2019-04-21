/**
 * Copyright (c) 2018 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

#include <stdint.h>
#include <stddef.h>

#include "CoreInput/input.h"
#include "input.h"

typedef struct{
    int event_write_pos;
    input_event_t event_ring[INPUT_EVENT_BUFFER_CNT];
} input_event_buffer_t;

int module_init() {
    //Create a ring buffer where incoming messages can be stored

    //Register input devices and input source (button/analog etc) sets

    //These inputs are passed on to the terminal system, which can then present them to applications as they require
    return 0;
}