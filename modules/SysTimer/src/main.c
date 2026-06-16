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
#include "SysMP/mp.h"
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

// Wait state for the periodic-interrupt fallback path of timer_wait(). This is
// PER-CORE (TLS): the fallback drives each core's own local timer, whose handler
// fires on that same core, so concurrent timer_wait() calls on different cores
// never share state. (The preferred path is the SMP-safe counter poll; this
// fallback is only taken on a platform with no readable counter timer.)
typedef struct
{
    _Atomic uint64_t pending;
    _Atomic uint64_t count;
    _Atomic uint64_t target;
} timer_wait_state_t;

static int timer_wait_tls_off = -1;

static timer_wait_state_t *timer_wait_self(void)
{
    return (timer_wait_state_t *)mp_tls_get(timer_wait_tls_off);
}

PRIVATE void timer_wait_handler(int irq)
{
    irq = 0;
    timer_wait_state_t *w = timer_wait_self();
    if (++w->count >= w->target && w->pending != 0)
    {
        DEBUG_PRINT("[SysTimer] Timer Wait Done\r\n");
        w->pending = 0;
    }
}

void timer_wait(uint64_t ns)
{
#define TIMER_WAIT_PERIODIC_INTR 1
#define TIMER_WAIT_COUNTER 2
    int idx = -1;
    int waitType = 0;

    // Prefer a readable counter (e.g. the calibrated TSC): polling it uses only
    // caller-stack state and no shared hardware, so it is SMP-safe -- any number
    // of cores can timer_wait() concurrently. The periodic-interrupt path below
    // is also per-core safe (it drives each core's own `local` timer with per-core
    // TLS wait state -- see timer_wait_state_t above), but it is only a fallback
    // for platforms with no readable counter timer.
    for (int i = 0; i < timer_idx; i++)
        if ((timer_defs[i].features & (timer_features_counter | timer_features_read)) &&
            timer_defs[i].handlers.read != NULL)
        {
            idx = i;
            waitType = TIMER_WAIT_COUNTER;
            break;
        }

    // Fallback: a per-core (local) periodic timer. It must be `local` so each
    // core drives its own hardware and its own TLS wait state; a shared periodic
    // timer could not serve concurrent waiters.
    if (idx < 0)
        for (int i = 0; i < timer_idx; i++)
            if ((timer_defs[i].features & timer_features_local) &&
                (timer_defs[i].features & timer_features_periodic) &&
                timer_defs[i].handlers.set_handler != NULL &&
                timer_defs[i].handlers.set_enable != NULL)
            {
                idx = i;
                waitType = TIMER_WAIT_PERIODIC_INTR;
                break;
            }

    if (idx < 0)
        PANIC("[SysTimer] Failed to select timer.");

    if (waitType == TIMER_WAIT_PERIODIC_INTR)
    {
        // Per-core wait state; the local timer's handler fires on this same core.
        timer_wait_state_t *w = timer_wait_self();
        w->count = 0;
        w->target = (ns * timer_defs[idx].handlers.rate) / (1000 * 1000 * 1000);
        if (w->target == 0)
            w->target = 1;
        w->pending = 1;
        DEBUG_PRINT("[SysTimer] Timer wait start!\r\n");

        timer_defs_t *t = &timer_defs[idx];
        DEBUG_PRINT("[SysTimer] Using local periodic timer: ");
        DEBUG_PRINT(t->handlers.name);
        DEBUG_PRINT("\r\n");

        // No in_use claim: a local timer is per-core, non-exclusive.
        t->handlers.set_handler(&t->handlers, timer_wait_handler);
        t->handlers.set_enable(&t->handlers, true);

        //Halt the cpu until this core's handler signals completion.
        while (w->pending)
            halt();

        t->handlers.set_enable(&t->handlers, false);

        DEBUG_PRINT("[SysTimer] Timer Finish\r\n");
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
        //Local timers are per-core hardware behind a single registration, so they
        //are not exclusive: every core may request the same entry and configure
        //its own (TLS-backed) state. Non-local timers stay first-come exclusive.
        if (((features & timer_features_local) || !timer_defs[idx].in_use) && ((timer_defs[idx].features & features) == features))
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

// Return the index of the first timer that is a readable, persistent counter,
// or -1 if none is available.
static int find_persistent_counter(void)
{
    for (int i = 0; i < timer_idx; i++)
        if ((timer_defs[i].features & (timer_features_read | timer_features_persistent | timer_features_counter)) == (timer_features_read | timer_features_persistent | timer_features_counter))
            if (timer_defs[i].handlers.read != NULL)
                return i;
    return -1;
}

uint64_t timer_timestamp()
{
    int idx = find_persistent_counter();
    if (idx < 0)
        return TIMER_NO_COUNTER;
    return timer_defs[idx].handlers.read(&timer_defs[idx].handlers);
}

// Persistent counter rate in ticks/sec (0 if no readable persistent counter).
uint64_t timer_counter_rate()
{
    int idx = find_persistent_counter();
    if (idx < 0)
        return 0;
    return timer_defs[idx].handlers.rate;
}

uint64_t timer_timestamp_ns()
{
    int idx = find_persistent_counter();
    if (idx < 0)
        return TIMER_NO_COUNTER;
    uint64_t rate = timer_defs[idx].handlers.rate;
    if (rate == 0)
        return TIMER_NO_COUNTER;
    // Integer ns = ticks/rate*1e9 + (ticks%rate)*1e9/rate. Kernel
    // modules build -mno-sse, so no floating point here; the split
    // avoids overflow (ticks*1e9 would wrap a few seconds after boot).
    uint64_t ticks = timer_defs[idx].handlers.read(&timer_defs[idx].handlers);
    return (ticks / rate) * 1000000000ULL + ((ticks % rate) * 1000000000ULL) / rate;
}

// Bounded busy-wait timeouts backed by the persistent counter (see timer.h).
// Safe with interrupts disabled (pure counter polling, no scheduler dependency),
// which is why drivers can use these from module_init where task_sleep cannot.
void timer_timeout_start(timer_timeout_t *t, uint64_t ns)
{
    uint64_t rate = timer_counter_rate();
    if (rate != 0)
    {
        uint64_t whole = ns / 1000000000ULL;
        uint64_t frac = ns % 1000000000ULL;
        t->timed = 1;
        // Round the fractional-second tick count UP: otherwise a sub-tick
        // timeout (e.g. US(500) on a low-rate counter) truncates to 0 ticks,
        // making the deadline equal to "now" so the timeout reports expired
        // immediately and busywaits don't actually wait.
        uint64_t frac_ticks = (frac * rate + 999999999ULL) / 1000000000ULL;
        t->deadline = timer_timestamp() + whole * rate + frac_ticks;
        t->spin = 0;
        t->spin_cap = 0;
    }
    else
    {
        // No calibrated time source: fall back to a bounded iteration count so
        // the loop still terminates (cpu-speed dependent, but never infinite).
        t->timed = 0;
        t->deadline = 0;
        t->spin = 0;
        t->spin_cap = 200000000ULL;
    }
}

int timer_timeout_expired(timer_timeout_t *t)
{
    if (t->timed)
        return timer_timestamp() >= t->deadline;
    return (t->spin++ >= t->spin_cap);
}

void timer_busywait_ns(uint64_t ns)
{
    timer_timeout_t t;
    timer_timeout_start(&t, ns);
    while (!timer_timeout_expired(&t))
        ;
}

void systimer_register_tests(void);

int module_init()
{
    // Reserve a per-core TLS slot for the periodic timer_wait() fallback state,
    // once on the BSP before any AP comes up (so every core's TLS includes it).
    timer_wait_tls_off = mp_tls_alloc(sizeof(timer_wait_state_t));

    timer_def_cnt = timer_platform_gettimercount();
    timer_defs = malloc(sizeof(timer_defs_t) * timer_def_cnt);
    memset(timer_defs, 0, sizeof(timer_defs_t) * timer_def_cnt);

    int err = timer_platform_init();
    if (err != 0)
        return err;

    systimer_register_tests();
    return 0;
}