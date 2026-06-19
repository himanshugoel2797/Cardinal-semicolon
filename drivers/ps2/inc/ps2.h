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

int PS2_Initialize();
uint8_t PS2_ReadStatus();
uint8_t PS2_ReadConfig();
void PS2_WriteConfig(uint8_t cfg);

// Async input service FFI (consumed by SysLisp's Lisp ps2 driver context).
// ps2_set_irq_hook installs an ISR-context callback the keyboard IRQ fires after
// queueing an event; ps2_poll_key dequeues one raw event; ps2_pending reports
// whether one is waiting.
void ps2_set_irq_hook(void (*hook)(void));
int ps2_poll_key(int *code, int *pressed);
int ps2_pending(void);

#endif /* end of include guard: _PS2_CTRL_H_ */
