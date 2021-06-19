// Copyright (c) 2021 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINALSEMI_USB_DRIV_H
#define CARDINALSEMI_USB_DRIV_H

#include <stdint.h>

typedef enum
{
    usb_device_type_unknown = 0,
    usb_device_type_uhci = 1,

    usb_device_type_count,
} usb_device_type_t;

typedef struct {
    char name[256];

    void *state;

    usb_device_type_t device_type;

    int lock;
} usb_hci_desc_t;

int usb_register_hostcontroller(usb_hci_desc_t *desc, void **handle);
int usb_device_connection_changed(void *handle, int port, bool connected);

#endif