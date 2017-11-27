// Copyright (c) 2017 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINAL_GLOBAL_FLAGS_H
#define CARDINAL_GLOBAL_FLAGS_H

typedef enum {
    GlobalFlags_None = 0,
    GlobalFlags_Interrupt = (1 << 0),   //Called from an interrupt context
} GlobalFlags_t;

#endif