// Copyright (c) 2018 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINAL_TIMER_H
#define CARDINAL_TIMER_H

#include <stdint.h>

typedef enum
{
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

// Returned by timer_timestamp()/timer_timestamp_ns() when no readable counter
// timer is registered. Callers that must tolerate that case should check for it.
#define TIMER_NO_COUNTER ((uint64_t)-1)

uint64_t timer_timestamp();

uint64_t timer_timestamp_ns();

// Ticks/sec of the persistent counter (TSC/HPET), or 0 if none is calibrated.
uint64_t timer_counter_rate();

// A bounded busy-wait timeout. Backed by the persistent counter when one is
// calibrated (a real, cpu-speed-independent wall-clock bound); otherwise it
// falls back to a bounded iteration count so the loop still always terminates.
// Pure polling -- safe to use with interrupts disabled (e.g. from a driver's
// module_init), unlike task_sleep. Usage:
//     timer_timeout_t to;
//     timer_timeout_start(&to, 500 * 1000 * 1000);   // 500ms
//     while (!device_ready())
//         if (timer_timeout_expired(&to)) { /* timed out */ break; }
typedef struct
{
    uint64_t deadline;  // counter ticks at which the timeout fires (when timed)
    uint64_t spin;      // fallback iteration counter (when no calibrated counter)
    uint64_t spin_cap;  // fallback iteration cap
    int timed;          // 1 = using the persistent counter, 0 = iteration fallback
} timer_timeout_t;

void timer_timeout_start(timer_timeout_t *t, uint64_t ns);
int timer_timeout_expired(timer_timeout_t *t);

// Busy-wait roughly `ns` nanoseconds (counter-paced when calibrated, else a
// bounded spin). For one-off hardware settle/recovery delays.
void timer_busywait_ns(uint64_t ns);

#endif