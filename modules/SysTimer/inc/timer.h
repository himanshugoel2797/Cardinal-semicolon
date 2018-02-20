// Copyright (c) 2017 Himanshu Goel
// 
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINAL_SYSTIMER_PRIV_H
#define CARDINAL_SYSTIMER_PRIV_H

#include <stdint.h>

typedef enum {
    timer_features_none = 0,
    timer_features_oneshot = (1 << 0),
    timer_features_periodic = (1 << 1),
    timer_features_read = (1 << 2),
    timer_features_persistent = (1 << 3),
    timer_features_absolute = (1 << 4),
    timer_features_64bit = (1 << 5),
    timer_features_write = (1 << 6),
    timer_features_local = (1 << 7),
    timer_features_pcie_msg_intr = (1 << 8),
    timer_features_fixed_intr = (1 << 9),
    timer_features_counter = (1 << 10),
} timer_features_t;

typedef struct {
    uint64_t (*read)(timer_handlers_t *);
    uint64_t (*write)(timer_handlers_t *, uint64_t);
    uint64_t (*set_mode)(timer_handlers_t *, timer_features_t);
    void (*set_enable)(timer_handlers_t *, bool);
    void (*set_handler)(timer_handlers_t *, void (*)());
    void (*send_eoi)(timer_handlers_t *);
    uint64_t rate;
    uint64_t state;
} timer_handlers_t;

int timer_register(timer_features_t features, timer_handlers_t *handlers);

int timer_platform_gettimercount();
int timer_platform_init();

#endif