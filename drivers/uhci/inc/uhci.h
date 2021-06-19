// Copyright (c) 2020 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINAL_SEMI_UHCI_STATE_H
#define CARDINAL_SEMI_UHCI_STATE_H

#include <stdint.h>

#define PORT_COUNT 2
#define FRAME_COUNT 1024

#define USBCMD_REG (0x00)
#define USBSTS_REG (0x02)
#define USBINTR_REG (0x04)
#define FRNUM_REG (0x06)
#define FRBASEADDR_REG (0x08)
#define SOFMOD_REG (0x0C)
#define PORTSCn_REG(x) (0x10 + (x * 2))

#define USBCMD_RS (1 << 0)
#define USBCMD_HCRESET (1 << 1)
#define USBCMD_GRESET (1 << 2)

#define USBSTS_USBINT (1 << 0)

#define PORTSC_CURCONNECT (1 << 0)
#define PORTSC_CONNECTCHG (1 << 1)
#define PORTSC_PORTEN (1 << 2)
#define PORTSC_PORTENCHG (1 << 3)
#define PORTSC_PORTRESET (1 << 9)

typedef struct uhci_framelist_entry {
    union{
        struct{
            uint32_t invalid : 1;
            uint32_t is_qh : 1;
            uint32_t zero : 2;
            uint32_t flp_31_4 : 28;
        };
        uint32_t flp;
    };
} uhci_framelist_entry_t;

typedef struct transfer_descriptor {
    union {
        struct{
            uint32_t invalid : 1;
            uint32_t is_qh : 1;
            uint32_t depth_first : 1;
            uint32_t zero : 1;
            uint32_t lp_31_4 : 28;
        };
        uint32_t lp;
    } link;
    struct {
        uint32_t act_len : 10;
        uint32_t rsv0 : 5;
        uint32_t status : 8;
        uint32_t ioc : 1;
        uint32_t ios : 1;
        uint32_t ls : 1;
        uint32_t err_count : 2;
        uint32_t spd : 1;
        uint32_t rsv1 : 2;
    } status;
    struct {
        uint32_t pid : 8;
        uint32_t device : 7;
        uint32_t endpoint : 4;
        uint32_t data_toggle : 1;
        uint32_t rsv0 : 1;
        uint32_t maxlen : 11;
    } token;
    uint32_t buffer_ptr;
} transfer_descriptor_t;

typedef struct uhci_ctrl_state
{
    uint32_t iobar;
    uint32_t framelist_pmem;
    uhci_framelist_entry_t *framelist;
    volatile bool init_complete;
    void *handle;
    cs_id intr_task;
    int id;

    struct uhci_ctrl_state *next;
} uhci_ctrl_state_t;

#endif