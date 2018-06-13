// Copyright (c) 2017 Himanshu Goel
// 
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINAL_SYSTIMER_PRIV_H
#define CARDINAL_SYSTIMER_PRIV_H

#include <stdint.h>

#include "SysTimer/timer.h"

typedef struct timer_handlers timer_handlers_t;
struct timer_handlers {
    char name[16];
    uint64_t (*read)(timer_handlers_t *);
    void (*write)(timer_handlers_t *, uint64_t);
    uint64_t (*set_mode)(timer_handlers_t *, timer_features_t);
    void (*set_enable)(timer_handlers_t *, bool);
    void (*set_handler)(timer_handlers_t *, void (*)(int));
    uint64_t rate;
    uint64_t state;
};

int timer_register(timer_features_t features, timer_handlers_t *handlers);

int timer_platform_gettimercount();
int timer_platform_init();

#endif