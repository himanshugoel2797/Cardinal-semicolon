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

static list_t device_handles_list;
static int device_handles_list_lock = 0;

static int devIDs[usb_device_type_count];

PRIVATE int module_init()
{
    list_init(&hostcontroller_list);
    list_init(&hostcontroller_handles_list);
    list_init(&device_handles_list);
    return 0;
}

static void usb_device_thread(usb_dev_def_t *device)
{
    while(true)
    {
        semaphore_wait(&device->sema);

        if(device->state == usb_device_state_disconnecting){
            device->state = usb_device_state_driver_disconnected;
            semaphore_signal(&device->sema);
            break; //exit this thread
        }

        semaphore_signal(&device->sema);
    }
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
        usb_hci_def_t *def = (usb_hci_def_t *)malloc(sizeof(usb_hci_def_t));
        def->type = devType;
        def->device = *desc;
        def->idx = devIDs[def->type]++;

        list_append(&hostcontroller_handles_list, def);
        *handle = def;
    }
    local_spinlock_unlock(&hostcontroller_handles_list_lock);

    return 0;
}

int usb_register_device(usb_device_t *desc, void **handle)
{
    usb_dev_def_t *def = NULL;
    
    local_spinlock_lock(&device_handles_list_lock);
    {
        def = (usb_dev_def_t *)malloc(sizeof(usb_dev_def_t));
        def->device = *desc;
        def->state = usb_device_state_uninitialized;
        def->idx = devIDs[def->type]++;
        def->dev_list_idx = list_len(&device_handles_list);
        semaphore_init(&def->sema);

        DEBUG_PRINT("[CoreUsb] Registered Device");
        create_task_kernel(def->device.name, task_permissions_kernel, &def->thread_id);

        list_append(&device_handles_list, def);
        *handle = def;
    }
    local_spinlock_unlock(&device_handles_list_lock);

    start_task_kernel(def->thread_id, usb_device_thread, def);
    semaphore_signal(&def->sema);

    return 0;
}

int usb_remove_device(void *handle)
{
    usb_dev_def_t *def = (usb_dev_def_t*)handle;
    semaphore_wait(&def->sema);
    def->state = usb_device_state_disconnecting;

    while(def->state != usb_device_state_driver_disconnected){
        semaphore_signal(&def->sema);
        task_yield();
        semaphore_wait(&def->sema);
    }

    //device thread has exited, clean up device entry
    local_spinlock_lock(&device_handles_list_lock);
    list_remove(&device_handles_list, def->dev_list_idx);
    local_spinlock_unlock(&device_handles_list_lock);

    free(def);

    return 0;
}

//TODO: CoreUsb spins up a new task to handle this device if connection or ends the task if disconnection
//TODO: CoreUsb can load up new modules to support different device types