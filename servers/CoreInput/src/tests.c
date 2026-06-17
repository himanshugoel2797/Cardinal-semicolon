// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include <types.h>

#include "SysTest/test.h"
#include "SysTaskMgr/task.h"
#include "CoreInput/input.h"

// --- A mock that actually produces events ------------------------------------
//
// CoreInput's background "core_input_updater" task continuously polls every
// registered device: for each device it calls has_pending(), and while that is
// true it calls read() and enqueues the event. There is no public accessor for
// the internal event queue, so we observe the poll/enqueue path through the
// mock's OWN callbacks: a bounded has_pending() that yields a fixed number of
// events and a read() that records it was driven. Once the poll task has called
// read() the expected number of times, we know the registered device was picked
// up and its events consumed end-to-end.
//
// has_pending() is bounded (it returns false after pending_left hits 0) so the
// poll loop can never spin filling the 16K event queue.

static volatile int mock_pending_left;   // events still "available"
static volatile int mock_reads;          // times read() was invoked
static volatile int mock_has_pending_calls;

static bool mock_has_pending(void *state)
{
    (void)state;
    mock_has_pending_calls++;
    return mock_pending_left > 0;
}

static void mock_read(void *state, input_device_event_t *event)
{
    (void)state;
    // Fill a plausible button event; avoid the float union member (no FP).
    event->timestamp = 0;
    event->is_btn_event = true;
    event->index = kbd_keys_A;
    event->state = true;
    if (mock_pending_left > 0)
        mock_pending_left--;
    mock_reads++;
}

// A mock whose has_pending is hardwired false: registers fine but never feeds
// the poll loop. Used by the register/unregister lifecycle tests.
static bool mock_has_pending_idle(void *state)
{
    (void)state;
    return false;
}

static input_device_desc_t make_mock_device(bool active)
{
    input_device_desc_t desc;
    desc.name[0] = 'm';
    desc.name[1] = 'o';
    desc.name[2] = 'c';
    desc.name[3] = 'k';
    desc.name[4] = '\0';
    desc.features = input_device_features_none;
    desc.handlers.has_pending = active ? mock_has_pending : mock_has_pending_idle;
    desc.handlers.read = mock_read;
    desc.type = input_device_type_keyboard;
    desc.state = NULL;
    return desc;
}

// --- register: success path + argument-guard rejections ----------------------

static void test_register_mock(test_ctx_t *ctx)
{
    // A NULL descriptor must be rejected.
    TEST_CHECK_EQ_U(ctx, input_device_register(NULL), (uint64_t)-1);

    // A descriptor missing has_pending must be rejected.
    input_device_desc_t bad = make_mock_device(false);
    bad.handlers.has_pending = NULL;
    TEST_CHECK_EQ_U(ctx, input_device_register(&bad), (uint64_t)-1);

    // A descriptor missing read must be rejected.
    bad = make_mock_device(false);
    bad.handlers.read = NULL;
    TEST_CHECK_EQ_U(ctx, input_device_register(&bad), (uint64_t)-1);

    // A well-formed descriptor registers successfully.
    input_device_desc_t desc = make_mock_device(false);
    TEST_CHECK_EQ_U(ctx, input_device_register(&desc), 0);

    // Clean up so the registration does not leak into the live device list.
    TEST_CHECK_EQ_U(ctx, input_device_unregister(&desc), 0);
}

// --- unregister: success path + absent-device path ---------------------------

static void test_unregister_mock(test_ctx_t *ctx)
{
    // Unregistering a never-registered device reports "not found" (returns 1),
    // not success and not an error.
    input_device_desc_t absent = make_mock_device(false);
    TEST_CHECK_EQ_U(ctx, input_device_unregister(&absent), 1);

    // A NULL descriptor is rejected.
    TEST_CHECK_EQ_U(ctx, input_device_unregister(NULL), (uint64_t)-1);

    // Register then unregister succeeds; a second unregister now reports absent.
    input_device_desc_t desc = make_mock_device(false);
    TEST_CHECK_EQ_U(ctx, input_device_register(&desc), 0);
    TEST_CHECK_EQ_U(ctx, input_device_unregister(&desc), 0);
    TEST_CHECK_EQ_U(ctx, input_device_unregister(&desc), 1);
}

// --- event poll/enqueue path -------------------------------------------------

// This MUST be a static so the descriptor outlives the test: the background
// poll task holds a pointer to it via the device list while it runs.
static input_device_desc_t poll_dev;

static void test_event_poll(test_ctx_t *ctx)
{
    const int want = 4;
    mock_pending_left = want;
    mock_reads = 0;
    mock_has_pending_calls = 0;

    poll_dev = make_mock_device(true);
    TEST_CHECK_EQ_U(ctx, input_device_register(&poll_dev), 0);

    // Give the background "core_input_updater" task time to poll the device and
    // drain its pending events. The loop exits as soon as all events are read, so
    // a healthy system still passes fast; the bound is generous (up to ~1s) so a
    // slow/loaded CI host (the full ~100-test suite under TCG) doesn't fail this
    // eventual-delivery check just because the poll task was scheduled less often.
    cs_id self = task_current();
    for (int i = 0; i < 200 && mock_reads < want; i++)
        task_sleep(self, MS(5));

    // The poll task must have driven our device: read() was called for each
    // event we made available, proving the registered device's events flow
    // through the poll/enqueue path end-to-end.
    TEST_CHECK_MSG(ctx, mock_has_pending_calls > 0,
                   "background poll task never polled the registered device");
    TEST_CHECK_EQ_U(ctx, mock_reads, want);

    // Stop further polling of this device before it (a stack/static) leaves
    // scope of the test's intent.
    TEST_CHECK_EQ_U(ctx, input_device_unregister(&poll_dev), 0);
}

void coreinput_register_tests(void)
{
    if (!test_mode_active())
        return;

    test_def_t reg = {
        .suite = "CoreInput",
        .name = "device_register_mock",
        .fn = test_register_mock,
        .run = TEST_RUN_INLINE,
        .flags = TEST_FLAG_NONE,
    };
    test_register(&reg);

    test_def_t unreg = {
        .suite = "CoreInput",
        .name = "device_unregister_mock",
        .fn = test_unregister_mock,
        .run = TEST_RUN_INLINE,
        .flags = TEST_FLAG_NONE,
    };
    test_register(&unreg);

    // Runs as a task so it can sleep while the background poll task makes
    // progress.
    test_def_t poll = {
        .suite = "CoreInput",
        .name = "event_poll_delivers",
        .fn = test_event_poll,
        .run = TEST_RUN_TASK,
        .flags = TEST_FLAG_NONE,
    };
    test_register(&poll);
}
