// Copyright (c) 2020 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINAL_SEMI_UHCI_STATE_H
#define CARDINAL_SEMI_UHCI_STATE_H

#include <stdint.h>

#define USBCMD_REG (0x00)
#define USBSTS_REG (0x02)
#define USBINTR_REG (0x04)
#define FRNUM_REG (0x06)
#define FRBASEADDR_REG (0x08)
#define SOFMOD_REG (0x0C)
#define PORTSCn_REG(x) (0x10 + (x * 2))

typedef struct uhci_ctrl_state
{
    uint32_t iobar;
    uint32_t framelist_pmem;
    uint32_t *framelist;

    struct uhci_ctrl_state *next;
} uhci_ctrl_state_t;

#endif