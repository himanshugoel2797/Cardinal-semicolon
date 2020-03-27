// Copyright (c) 2018 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef _PS2_CTRL_H_
#define _PS2_CTRL_H_

#include <stdint.h>

#include "ps2_keyboard.h"
#include "ps2_mouse.h"

#define BUF_LEN 8192
#define ENT_SIZE 4

uint8_t PS2_Initialize();
uint8_t PS2_ReadStatus();
uint8_t PS2_ReadConfig();
void PS2_WriteConfig(uint8_t cfg);

#endif /* end of include guard: _PS2_CTRL_H_ */
