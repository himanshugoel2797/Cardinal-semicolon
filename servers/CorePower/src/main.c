/**
 * Copyright (c) 2018 Himanshu Goel
 * 
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

#include <stdqueue.h>
#include <cardinal/local_spinlock.h>

#include "CorePower/power.h"

static queue_t devices;
static int devices_lock;

//Register a power management device
int pwr_register(pwr_device_t *device) {
    int retVal = 0;

    local_spinlock_lock(&devices_lock);
    if(!queue_tryenqueue(&devices, (uint64_t)device))
        retVal = -1;
    local_spinlock_unlock(&devices_lock);
    
    return retVal;
}

//Send a power state change event
int pwr_sendevent_g(device_pwr_class_t pwr_class, global_pwr_state_t state, int p_state) {

    local_spinlock_lock(&devices_lock);
    
    pwr_device_t *device = NULL;
    int entcnt = queue_entcnt(&devices);
    for(int i = 0; i < entcnt; i++)
        if(queue_trydequeue(&devices, (uint64_t*)device)){
            if(device->event_g != NULL && (device->dev_class & pwr_class))
                device->event_g(state, p_state);
            queue_tryenqueue(&devices, (uint64_t)device);
        }

    local_spinlock_unlock(&devices_lock);
    return 0;
}
int pwr_sendevent_d(device_pwr_class_t pwr_class, device_pwr_state_t state) {
    
    local_spinlock_lock(&devices_lock);
    
    pwr_device_t *device = NULL;
    int entcnt = queue_entcnt(&devices);
    for(int i = 0; i < entcnt; i++)
        if(queue_trydequeue(&devices, (uint64_t*)device)){
            if(device->event_d != NULL && (device->dev_class & pwr_class))
                device->event_d(state);
            queue_tryenqueue(&devices, (uint64_t)device);
        }

    local_spinlock_unlock(&devices_lock);
    return 0;
}

int module_init(){
    local_spinlock_lock(&devices_lock);

    queue_init(&devices, 1024);

    local_spinlock_unlock(&devices_lock);
    return 0;
}