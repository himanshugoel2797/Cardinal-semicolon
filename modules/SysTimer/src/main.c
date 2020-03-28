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

#include "SysTimer/timer.h"
#include "timer.h"

typedef struct
{
    bool in_use;
    timer_features_t features;
    timer_handlers_t handlers;
    void (*cur_handler)(int);
} timer_defs_t;

static timer_defs_t *timer_defs = NULL;
static int timer_def_cnt = 0;
static int timer_idx = 0;

PRIVATE int timer_register(timer_features_t features, timer_handlers_t *handlers)
{
    memcpy(&timer_defs[timer_idx].handlers, handlers, sizeof(timer_handlers_t));
    timer_defs[timer_idx].features = features;
    DEBUG_PRINT("[SysTimer] Register Timer: ");
    DEBUG_PRINT(handlers->name);
    DEBUG_PRINT("\r\n");
    return timer_idx++;
}

//TODO Figure out how to handle smp timer usage
static _Atomic uint64_t timer_wait_pending = 0;
static _Atomic uint64_t timer_wait_count = 0;
static _Atomic uint64_t timer_wait_target = 0;
static timer_defs_t *timer_wait_d = NULL;
PRIVATE void timer_wait_handler(int irq)
{
    irq = 0;
    if (++timer_wait_count >= timer_wait_target && timer_wait_pending != 0)
    {
        DEBUG_PRINT("[SysTimer] Timer Wait Done\r\n");
        timer_wait_pending = 0;
    }
}

void timer_wait(uint64_t ns)
{
#define TIMER_WAIT_PERIODIC_INTR 1
#define TIMER_WAIT_COUNTER 2
    //Allocate a timer for oneshot mode with a rate that can match the desired time
    int idx = 0;
    int waitType = 0;
    for (; idx < timer_idx; idx++)
        if (!timer_defs[idx].in_use) // && (timer_defs[idx].features & timer_features_periodic))
        {
            if (timer_defs[idx].features & timer_features_periodic)
            {
                if (timer_defs[idx].handlers.set_handler != NULL &&
                    timer_defs[idx].handlers.set_enable != NULL)
                {
                    waitType = TIMER_WAIT_PERIODIC_INTR;
                    break;
                }
            }
            else if (timer_defs[idx].features & (timer_features_counter | timer_features_read))
            {
                if (timer_defs[idx].handlers.read != NULL)
                {
                    waitType = TIMER_WAIT_COUNTER;
                    break;
                }
            }
        }
    if (idx == timer_idx)
        PANIC("[SysTimer] Failed to select timer.");

    if (waitType == TIMER_WAIT_PERIODIC_INTR)
    {
        timer_wait_count = 0;
        timer_wait_pending = 1;
        DEBUG_PRINT("[SysTimer] Timer wait start!\r\n");

        //Configure it for oneshot wait handler
        timer_defs_t *t = &timer_defs[idx];
        timer_wait_d = t;

        timer_wait_target = (ns * t->handlers.rate) / (1000 * 1000 * 1000);

        if (timer_wait_target == 0)
            timer_wait_target = 1;

        DEBUG_PRINT("[SysTimer] Allocated one-shot timer: ");
        DEBUG_PRINT(t->handlers.name);
        DEBUG_PRINT("\r\n");

        t->in_use = true;
        //t->handlers.set_mode(&t->handlers, timer_features_oneshot);
        t->handlers.set_handler(&t->handlers, timer_wait_handler);
        t->handlers.set_enable(&t->handlers, true);

        //Halt the cpu
        while (timer_wait_pending)
            halt();

        t->handlers.set_enable(&t->handlers, false);

        DEBUG_PRINT("[SysTimer] Timer Finish\r\n");

        timer_wait_d = NULL;
        t->in_use = false;
    }
    else if (waitType == TIMER_WAIT_COUNTER)
    {
        DEBUG_PRINT("[SysTimer] Using counter poll for sleep.\r\n");

        timer_defs_t *t = &timer_defs[idx];
        uint64_t target_val = (ns * t->handlers.rate) / (1000 * 1000 * 1000);
        target_val += t->handlers.read(&t->handlers);
        while (target_val > t->handlers.read(&t->handlers))
            ;

        DEBUG_PRINT("[SysTimer] Timer Finish\r\n");
    }
}

int timer_request(timer_features_t features, uint64_t ns, void (*handler)(int))
{
    //Allocate a timer for the desired mode with a rate that can match the desired time
    int idx = 0;
    for (; idx < timer_idx; idx++)
        if (!timer_defs[idx].in_use && ((timer_defs[idx].features & features) == features))
        {

            if (timer_defs[idx].handlers.set_mode != NULL &&
                timer_defs[idx].handlers.set_handler != NULL &&
                timer_defs[idx].handlers.set_enable != NULL)
            {

                if ((features & timer_features_write) && (timer_defs[idx].handlers.write != NULL))
                    break;
                else if (!(features & timer_features_write))
                    break;
            }
        }

    if (idx == timer_idx)
        return -1;

    //Configure the timer
    timer_defs_t *t = &timer_defs[idx];
    timer_wait_d = t;
    t->in_use = true;
    t->handlers.set_mode(&t->handlers, features);
    if (features & timer_features_write)
        t->handlers.write(&t->handlers, (ns * t->handlers.rate) / (1000 * 1000 * 1000));
    t->handlers.set_handler(&t->handlers, handler);
    t->handlers.set_enable(&t->handlers, true);

    print_str("[SysTimer] Allocated timer: ");
    print_str(t->handlers.name);
    print_str("\r\n");

    return 0;
}

uint64_t timer_timestamp()
{
    //Allocate a timer for the desired mode with a rate that can match the desired time
    int idx = 0;
    for (; idx < timer_idx; idx++)
        if ((timer_defs[idx].features & (timer_features_read | timer_features_persistent | timer_features_counter)) == (timer_features_read | timer_features_persistent | timer_features_counter))
            if (timer_defs[idx].handlers.read != NULL)
                return timer_defs[idx].handlers.read(&timer_defs[idx].handlers);

    return (uint64_t)-1;
}

static int timer_init()
{
    return 0;
}

int module_init()
{
    timer_def_cnt = timer_platform_gettimercount();
    timer_defs = malloc(sizeof(timer_defs_t) * timer_def_cnt);
    memset(timer_defs, 0, sizeof(timer_defs_t) * timer_def_cnt);

    int err = 0;

    err = timer_platform_init();
    if (err != 0)
        return err;

    //callibrate timers as needed
    err = timer_init();

    return err;
}