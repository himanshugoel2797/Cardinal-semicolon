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

static list_t hostcontroller_handles_list;
static int hostcontroller_handles_list_lock = 0;

static list_t device_list;
static int device_list_lock = 0;

static list_t device_handles_list;
static int device_handles_list_lock = 0;

static int devIDs[usb_device_type_count];

PRIVATE int module_init()
{
    list_init(&hostcontroller_list);
    list_init(&hostcontroller_handles_list);
    list_init(&device_list);
    list_init(&device_handles_list);
    return 0;
}

int usb_register_hostcontroller(usb_hci_desc_t *desc, void **handle)
{
    usb_device_type_t devType;
    local_spinlock_lock(&hostcontroller_list_lock);
    local_spinlock_lock(&desc->lock);
    {
        list_append(&hostcontroller_list, desc);

        devType = desc->device_type;
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
    
    local_spinlock_lock(&hostcontroller_handles_list_lock);
    {
        list_append(&hostcontroller_list, desc);

        usb_hci_def_t *def = (usb_hci_def_t *)malloc(sizeof(usb_hci_def_t));
        def->type = devType;
        def->device = *desc;
        def->idx = devIDs[def->type]++;

        *handle = def;
    }
    local_spinlock_unlock(&hostcontroller_handles_list_lock);

    return 0;
}

int usb_register_device(usb_device_t *desc, void **handle)
{
    local_spinlock_lock(&device_list_lock);
    local_spinlock_lock(&desc->lock);
    {
        list_append(&device_list, desc);

        DEBUG_PRINT("[CoreUsb] Registered Device");
    }
    local_spinlock_unlock(&desc->lock);
    local_spinlock_unlock(&device_list_lock);
    
    local_spinlock_lock(&device_handles_list_lock);
    {
        list_append(&device_list, desc);

        usb_dev_def_t *def = (usb_dev_def_t *)malloc(sizeof(usb_dev_def_t));
        def->device = *desc;
        def->state = usb_device_state_uninitialized;
        def->idx = devIDs[def->type]++;

        *handle = def;
    }
    local_spinlock_unlock(&device_handles_list_lock);

    return 0;
}

int usb_remove_device(void *handle)
{
    handle = NULL;
    return 0;
}

//TODO: CoreUsb spins up a new task to handle this device if connection or ends the task if disconnection
//TODO: CoreUsb can load up new modules to support different device types