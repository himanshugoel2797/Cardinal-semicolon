// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#include <stddef.h>

#include "SysTest/test.h"
#include "CorePower/power.h"

// pwr_register with a minimal mock device (null handlers) returns 0.
static void test_pwr_register(test_ctx_t *ctx) {
    pwr_device_t dev = {
        .name = "test",
        .event_g = NULL,
        .event_d = NULL,
        .cur_pstate = 0,
        .cur_dstate = d0,
        .cur_gstate = g0_pXX,
        .dev_class = generic,
    };

    TEST_CHECK_EQ_U(ctx, pwr_register(&dev), 0);
}

// pwr_sendevent_g dispatches a global power-state change without faulting on a
// device with a null handler.
static void test_pwr_sendevent_g(test_ctx_t *ctx) {
    pwr_device_t dev = {
        .name = "test",
        .event_g = NULL,
        .event_d = NULL,
        .cur_pstate = 0,
        .cur_dstate = d0,
        .cur_gstate = g0_pXX,
        .dev_class = generic,
    };

    TEST_CHECK_EQ_U(ctx, pwr_register(&dev), 0);
    TEST_CHECK_EQ_U(ctx, pwr_sendevent_g(generic, g0_pXX, 0), 0);
}

// pwr_sendevent_d dispatches a device power-state change without faulting on a
// device with a null handler.
static void test_pwr_sendevent_d(test_ctx_t *ctx) {
    pwr_device_t dev = {
        .name = "test",
        .event_g = NULL,
        .event_d = NULL,
        .cur_pstate = 0,
        .cur_dstate = d0,
        .cur_gstate = g0_pXX,
        .dev_class = generic,
    };

    TEST_CHECK_EQ_U(ctx, pwr_register(&dev), 0);
    TEST_CHECK_EQ_U(ctx, pwr_sendevent_d(generic, d0), 0);
}

void corepower_register_tests(void) {
    if (!test_mode_active())
        return;

    {
        test_def_t t = {
            .suite = "CorePower",
            .name = "pwr_register",
            .fn = test_pwr_register,
            .run = TEST_RUN_INLINE,
            .flags = TEST_FLAG_NONE,
        };
        test_register(&t);
    }

    {
        test_def_t t = {
            .suite = "CorePower",
            .name = "pwr_sendevent_g",
            .fn = test_pwr_sendevent_g,
            .run = TEST_RUN_INLINE,
            .flags = TEST_FLAG_NONE,
        };
        test_register(&t);
    }

    {
        test_def_t t = {
            .suite = "CorePower",
            .name = "pwr_sendevent_d",
            .fn = test_pwr_sendevent_d,
            .run = TEST_RUN_INLINE,
            .flags = TEST_FLAG_NONE,
        };
        test_register(&t);
    }
}
