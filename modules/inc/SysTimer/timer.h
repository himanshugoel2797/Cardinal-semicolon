// Copyright (c) 2018 Himanshu Goel
// 
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINAL_TIMER_H
#define CARDINAL_TIMER_H

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

void timer_wait(uint64_t ns);

int timer_request(timer_features_t features, uint64_t ns, void (*handler)(int));

#endif