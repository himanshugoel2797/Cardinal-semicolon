/**
 * Copyright (c) 2018 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdlist.h>
#include <stdqueue.h>
#include <cardinal/local_spinlock.h>

#include "SysTaskMgr/task.h"
#include "CoreInput/input.h"
#include "input.h"

static int device_list_lock = 0;
static list_t device_list;

static int event_lock = 0;
static queue_t event_queue;

int input_device_register(input_device_desc_t *desc)
{
    if (desc == NULL)
        return -1;

    if (desc->handlers.has_pending == NULL)
        return -1;

    if (desc->handlers.read == NULL)
        return -1;

    local_spinlock_lock(&device_list_lock);
    if (list_append(&device_list, desc) != list_error_none)
    {
        local_spinlock_unlock(&device_list_lock);
        return -2;
    }

    DEBUG_PRINT("[CoreInput] Registered Device:");
    DEBUG_PRINT(desc->name);
    DEBUG_PRINT("\r\n");

    local_spinlock_unlock(&device_list_lock);
    return 0;
}

int input_device_unregister(input_device_desc_t *desc)
{
    if (desc == NULL)
        return -1;

    local_spinlock_lock(&device_list_lock);
    int len = (int)list_len(&device_list);

    for (int i = 0; i < len; i++)
    {
        if ((uintptr_t)list_at(&device_list, (uint64_t)i) == (uintptr_t)desc)
        {
            list_remove(&device_list, (uint64_t)i);
            local_spinlock_unlock(&device_list_lock);
            return 0;
        }
    }

    local_spinlock_unlock(&device_list_lock);
    return 1;
}

static void read_devices(void *arg)
{
    arg = NULL;

    while (1)
    {
        //Read all registered devices
        local_spinlock_lock(&device_list_lock);
        int len = (int)list_len(&device_list);

        for (int i = 0; i < len; i++)
        {
            input_device_desc_t *desc = (input_device_desc_t *)list_at(&device_list, (uint64_t)i);

            while (desc->handlers.has_pending(desc->state))
            {
                if (queue_full(&event_queue))
                    break; //Queue is full

                input_device_event_t *event = malloc(sizeof(input_device_event_t));
                desc->handlers.read(desc->state, event);
                if (!queue_tryenqueue(&event_queue, (uint64_t)event))
                {
                    free(event);
                    break;
                }
            }
        }
        //perform an inplace sort based on the timestamp
        local_spinlock_unlock(&device_list_lock);
    }
}

int module_init()
{
    local_spinlock_lock(&device_list_lock);
    if (list_init(&device_list) != 0)
        return -1;
    local_spinlock_unlock(&device_list_lock);

    local_spinlock_lock(&event_lock);
    if (queue_init(&event_queue, INPUT_EVENT_BUFFER_CNT) != 0)
        return -1;
    local_spinlock_unlock(&event_lock);

    cs_id pid = 0;
    create_task_kernel("core_input_updater", task_permissions_kernel, &pid);
    start_task_kernel(pid, read_devices, NULL);

    return 0;
}