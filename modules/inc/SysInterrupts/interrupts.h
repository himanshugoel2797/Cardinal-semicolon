// Copyright (c) 2017 Himanshu Goel
// 
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINAL_INTERRUPTS_H
#define CARDINAL_INTERRUPTS_H

typedef void (*InterruptHandler)();

void interrupt_registerhandler(int idx, InterruptHandler hndlr);

#endif