// Copyright (c) 2020 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINAL_SEMI_UHCI_STATE_H
#define CARDINAL_SEMI_UHCI_STATE_H

#include <stdint.h>

typedef struct uhci_ctrl_state
{
    uint32_t iobar;

    struct uhci_ctrl_state *next;
} uhci_ctrl_state_t;

#endif