/**
 * Copyright (c) 2021 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

#include <stdint.h>
#include <stdlib.h>
#include <stdlist.h>
#include <cardinal/local_spinlock.h>

#include "usb_priv.h"
#include "CoreUsb/usb.h"

static list_t hostcontroller_list;
static int hostcontroller_list_lock = 0;

PRIVATE int module_init()
{
    list_init(&hostcontroller_list);
    return 0;
}

int usb_register_hostcontroller(usb_hci_desc_t *desc, void **handle)
{
    local_spinlock_lock(&hostcontroller_list_lock);
    local_spinlock_lock(&desc->lock);
    {
        list_append(&hostcontroller_list, desc);

        DEBUG_PRINT("[CoreUsb] Registered ");
        switch(desc->device_type)
        {
            case usb_device_type_uhci:
                DEBUG_PRINT("UHCI: ");
                break;
            default:
                DEBUG_PRINT("unknown: ");
                break;
        }
        DEBUG_PRINT(desc->name);
        DEBUG_PRINT("\r\n");

        *handle = desc;
    }
    local_spinlock_unlock(&desc->lock);
    local_spinlock_unlock(&hostcontroller_list_lock);

    return 0;
}