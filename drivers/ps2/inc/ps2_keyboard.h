// Copyright (c) 2018 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef _PS2_KEYBOARD_H_
#define _PS2_KEYBOARD_H_

#include <stdint.h>

uint8_t PS2Keyboard_Initialize();
void PS2Keyboard_SetLEDStatus(uint8_t led, uint8_t status);
int PS2Keyboard_ActiveScancodeSet();

#endif /* end of include guard: _PS2_KEYBOARD_H_ */