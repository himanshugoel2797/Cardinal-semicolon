// Copyright (c) 2018 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef _PS2_MOUSE_DRIVER_H_
#define _PS2_MOUSE_DRIVER_H_

#include <stdint.h>

uint8_t
PS2Mouse_Initialize(void);

bool PS2Mouse_IsFiveButton(void);

bool PS2Mouse_HasScrollWheel(void);

#endif