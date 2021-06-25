// Copyright (c) 2021 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINALSEMI_USB_PRIV_DRIV_H
#define CARDINALSEMI_USB_PRIV_DRIV_H

#include <stdint.h>
#include "CoreUsb/usb.h"
#include "SysTaskMgr/task.h"

typedef struct {
    usb_device_type_t type;
    usb_hci_desc_t device;
    int idx;
} usb_hci_def_t;

typedef struct {
    usb_device_type_t type;
    usb_device_t device;
    volatile usb_device_state_t state;
    int idx;
    uint32_t dev_list_idx;
    cs_id thread_id;
    semaphore_t sema;
} usb_dev_def_t;

#endif