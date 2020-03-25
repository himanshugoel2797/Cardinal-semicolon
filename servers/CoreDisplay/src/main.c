/**
 * Copyright (c) 2018 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

#include <stdlib.h>
#include <stdlist.h>
#include <cardinal/local_spinlock.h>

#include "load_script.h"

#include "CoreDisplay/display.h"

static int display_list_lock = 0;
static list_t display_list;

//register display devices
int display_register(display_desc_t *desc)
{
    if (desc == NULL)
        return -1;

    local_spinlock_lock(&display_list_lock);
    if (list_append(&display_list, desc) != list_error_none)
    {
        local_spinlock_unlock(&display_list_lock);
        return -2;
    }

    DEBUG_PRINT("[CoreDisplay] Registered Device: ");
    DEBUG_PRINT(desc->display_name);
    DEBUG_PRINT("\r\n");

    local_spinlock_unlock(&display_list_lock);
    return 0;
}

//deregister display devices
int display_unregister(display_desc_t *desc)
{
    if (desc == NULL)
        return -1;

    local_spinlock_lock(&display_list_lock);
    int len = (int)list_len(&display_list);

    for (int i = 0; i < len; i++)
    {
        if ((uintptr_t)list_at(&display_list, (uint64_t)i) == (uintptr_t)desc)
        {
            list_remove(&display_list, (uint64_t)i);
            local_spinlock_unlock(&display_list_lock);
            return 0;
        }
    }

    local_spinlock_unlock(&display_list_lock);
    return 1;
}

//get info/handlers

int coredisplay_postinit()
{
    local_spinlock_lock(&display_list_lock);
    uint64_t len = list_len(&display_list);
    local_spinlock_unlock(&display_list_lock);

    if (len == 0)
    {
        //Load the lfb driver
        if (module_load("./lfb.celf") != 0)
            PANIC("[CoreDisplay] No framebuffer available!\r\n");
    }

    return 0;
}

int module_init()
{
    local_spinlock_lock(&display_list_lock);
    if (list_init(&display_list) != 0)
        return -1;

    local_spinlock_unlock(&display_list_lock);

    return 0;
}