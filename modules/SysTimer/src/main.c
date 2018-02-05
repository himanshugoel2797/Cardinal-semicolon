/**
 * Copyright (c) 2017 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>
#include <types.h>

#include "timer.h"

typedef struct {
    bool callibrated;
    timer_features_t features;
    timer_handlers_t handlers;
} timer_defs_t;

static timer_defs_t *timer_defs = NULL;
static int timer_def_cnt = 0;
static int timer_idx = 0;

UNUSED static int timer_register(timer_features_t features, timer_handlers_t *handlers) {
    memcpy(&timer_defs[timer_idx].handlers, handlers, sizeof(timer_handlers_t));
    timer_defs[timer_idx].features = features;
    return timer_idx++;
}

static int timer_init(){
    return 0;
}

int module_init() {
    timer_def_cnt = timer_platform_gettimercount() + 1;
    timer_defs = malloc(sizeof(timer_defs_t) * timer_def_cnt);

    int err = 0;
    
    err = timer_platform_init();
    if(err != 0)
        return err;

    //callibrate timers as needed
    err = timer_init();


    return err;
}