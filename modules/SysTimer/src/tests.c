// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#include <stdint.h>
#include <stdbool.h>
#include <types.h>

#include "SysTimer/timer.h"
#include "SysTest/test.h"

// The persistent counter (TSC/HPET) must be calibrated for any sane time source.
static void test_counter_rate_nonzero(test_ctx_t *ctx)
{
    TEST_CHECK_MSG(ctx, timer_counter_rate() > 0,
                   "timer_counter_rate() must be > 0");
}

// A readable counter must be present so timestamps are meaningful.
static void test_timestamp_available(test_ctx_t *ctx)
{
    TEST_CHECK_MSG(ctx, timer_timestamp_ns() != TIMER_NO_COUNTER,
                   "timer_timestamp_ns() must not be TIMER_NO_COUNTER");
}

// Timestamps must never go backwards.
static void test_timestamp_monotonic(test_ctx_t *ctx)
{
    uint64_t t0 = timer_timestamp_ns();
    uint64_t t1 = timer_timestamp_ns();
    TEST_CHECK_MSG(ctx, t1 >= t0,
                   "timer_timestamp_ns() must be monotonic");
}

// A started timeout must eventually report expired (the loop must terminate).
static void test_timeout_terminates(test_ctx_t *ctx)
{
    timer_timeout_t t;
    timer_timeout_start(&t, US(500));
    while (!timer_timeout_expired(&t))
        ;
    // Reaching here means the timeout terminated; record a passing check.
    TEST_CHECK(ctx, timer_timeout_expired(&t));
}

// A short busy-wait must complete without hanging or faulting.
static void test_busywait_smoke(test_ctx_t *ctx)
{
    uint64_t t0 = timer_timestamp_ns();
    timer_busywait_ns(1000);
    uint64_t t1 = timer_timestamp_ns();
    TEST_CHECK_MSG(ctx, t1 >= t0,
                   "timer_busywait_ns() returned and time did not regress");
}

void systimer_register_tests(void)
{
    if (!test_mode_active())
        return;

    {
        test_def_t t = {
            .suite = "SysTimer",
            .name = "counter_rate_nonzero",
            .fn = test_counter_rate_nonzero,
            .run = TEST_RUN_INLINE,
            .flags = TEST_FLAG_NONE,
        };
        test_register(&t);
    }
    {
        test_def_t t = {
            .suite = "SysTimer",
            .name = "timestamp_available",
            .fn = test_timestamp_available,
            .run = TEST_RUN_INLINE,
            .flags = TEST_FLAG_NONE,
        };
        test_register(&t);
    }
    {
        test_def_t t = {
            .suite = "SysTimer",
            .name = "timestamp_monotonic",
            .fn = test_timestamp_monotonic,
            .run = TEST_RUN_INLINE,
            .flags = TEST_FLAG_NONE,
        };
        test_register(&t);
    }
    {
        test_def_t t = {
            .suite = "SysTimer",
            .name = "timeout_terminates",
            .fn = test_timeout_terminates,
            .run = TEST_RUN_INLINE,
            .flags = TEST_FLAG_NONE,
        };
        test_register(&t);
    }
    {
        test_def_t t = {
            .suite = "SysTimer",
            .name = "busywait_smoke",
            .fn = test_busywait_smoke,
            .run = TEST_RUN_INLINE,
            .flags = TEST_FLAG_NONE,
        };
        test_register(&t);
    }
}
