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
    usb_device_type_ehci = 2,
    usb_device_type_ohci = 3,
    usb_device_type_xhci = 4,
    
    usb_device_type_hub = 5,

    usb_device_type_count,
} usb_device_type_t;

typedef enum
{
    usb_device_state_uninitialized,
    usb_device_state_disconnecting,
    usb_device_state_driver_disconnected,
} usb_device_state_t;

typedef struct {
    char name[256];

    void *state;

    usb_device_type_t device_type;

    int lock;
} usb_hci_desc_t;

typedef struct {
    char name[256];
    uint32_t hci_id;
    uint32_t address;
    uint32_t port;
    uint32_t speed;

    int lock;
} usb_device_t;

int usb_register_hostcontroller(usb_hci_desc_t *desc, void **handle);
int usb_register_device(usb_device_t *desc, void **handle);
int usb_remove_device(void *handle);

#endif