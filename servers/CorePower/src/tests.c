// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#include <stddef.h>

#include "SysTest/test.h"
#include "CorePower/power.h"

// NOTE on lifetime: pwr_register() stores the pwr_device_t* in a global queue
// that is never drained, and pwr_sendevent_*() walks every registered device on
// each call. So the mock devices and their callbacks MUST outlive the test --
// they are 'static' here, not stack temporaries, or a later event would chase a
// dangling pointer. Each test also uses its OWN device class so that one test's
// persistently-registered device cannot be matched (and its flag re-fired) by a
// later test's dispatch.

// --- pwr_register ------------------------------------------------------------

// Registering a device must succeed; registering it again must also succeed and
// leave it discoverable (an observable effect: a class-matched global event
// reaches its handler).
static volatile int reg_g_fired;
static int reg_event_g(global_pwr_state_t tgt_state, int p_state) {
    (void)tgt_state;
    (void)p_state;
    reg_g_fired = 1;
    return 0;
}
static pwr_device_t reg_dev = {
    .name = "test_reg",
    .event_g = reg_event_g,
    .event_d = NULL,
    .cur_pstate = 0,
    .cur_dstate = d0,
    .cur_gstate = g0_pXX,
    .dev_class = processor,
};

static void test_pwr_register(test_ctx_t *ctx) {
    TEST_CHECK_EQ_U(ctx, pwr_register(&reg_dev), 0);

    // Observable registration effect: a global event for our class reaches the
    // handler, proving the device is actually in the dispatch set.
    reg_g_fired = 0;
    TEST_CHECK_EQ_U(ctx, pwr_sendevent_g(processor, g1_s3, 0), 0);
    TEST_CHECK_MSG(ctx, reg_g_fired == 1,
                   "registered device did not receive a class-matched global event");
}

// --- pwr_sendevent_g ---------------------------------------------------------

// Dispatching a global power event must invoke the matching device's event_g
// with the requested state/p_state.
static volatile int g_fired;
static volatile global_pwr_state_t g_seen_state;
static volatile int g_seen_pstate;
static int mock_event_g(global_pwr_state_t tgt_state, int p_state) {
    g_fired = 1;
    g_seen_state = tgt_state;
    g_seen_pstate = p_state;
    return 0;
}
static pwr_device_t g_dev = {
    .name = "test_g",
    .event_g = mock_event_g,
    .event_d = NULL,
    .cur_pstate = 0,
    .cur_dstate = d0,
    .cur_gstate = g0_pXX,
    .dev_class = display,
};

static void test_pwr_sendevent_g(test_ctx_t *ctx) {
    TEST_CHECK_EQ_U(ctx, pwr_register(&g_dev), 0);

    g_fired = 0;
    g_seen_state = g0_pXX;
    g_seen_pstate = -1;
    TEST_CHECK_EQ_U(ctx, pwr_sendevent_g(display, g1_s4, 3), 0);

    // The callback must have fired and received exactly what we dispatched.
    TEST_CHECK_MSG(ctx, g_fired == 1, "event_g handler was not invoked");
    TEST_CHECK_EQ_U(ctx, g_seen_state, g1_s4);
    TEST_CHECK_EQ_U(ctx, g_seen_pstate, 3);

    // A non-matching class must NOT invoke our handler.
    g_fired = 0;
    TEST_CHECK_EQ_U(ctx, pwr_sendevent_g(audio_in, g1_s4, 0), 0);
    TEST_CHECK_MSG(ctx, g_fired == 0,
                   "event_g handler fired for a non-matching device class");
}

// --- pwr_sendevent_d ---------------------------------------------------------

// Dispatching a device power event must invoke the matching device's event_d
// with the requested state.
static volatile int d_fired;
static volatile device_pwr_state_t d_seen_state;
static int mock_event_d(device_pwr_state_t tgt_state) {
    d_fired = 1;
    d_seen_state = tgt_state;
    return 0;
}
static pwr_device_t d_dev = {
    .name = "test_d",
    .event_g = NULL,
    .event_d = mock_event_d,
    .cur_pstate = 0,
    .cur_dstate = d0,
    .cur_gstate = g0_pXX,
    .dev_class = audio_out,
};

static void test_pwr_sendevent_d(test_ctx_t *ctx) {
    TEST_CHECK_EQ_U(ctx, pwr_register(&d_dev), 0);

    d_fired = 0;
    d_seen_state = d0;
    TEST_CHECK_EQ_U(ctx, pwr_sendevent_d(audio_out, d3), 0);

    // The callback must have fired and received exactly what we dispatched.
    TEST_CHECK_MSG(ctx, d_fired == 1, "event_d handler was not invoked");
    TEST_CHECK_EQ_U(ctx, d_seen_state, d3);

    // A non-matching class must NOT invoke our handler.
    d_fired = 0;
    TEST_CHECK_EQ_U(ctx, pwr_sendevent_d(camera, d3), 0);
    TEST_CHECK_MSG(ctx, d_fired == 0,
                   "event_d handler fired for a non-matching device class");
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
