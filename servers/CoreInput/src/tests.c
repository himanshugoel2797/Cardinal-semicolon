// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#include <stddef.h>

#include "SysTest/test.h"
#include "CoreInput/input.h"

// Mock device handlers: no pending events, read is a no-op.
static bool mock_has_pending(void *state)
{
    (void)state;
    return false;
}

static void mock_read(void *state, input_device_event_t *events)
{
    (void)state;
    (void)events;
}

static input_device_desc_t make_mock_device(void)
{
    input_device_desc_t desc;
    desc.name[0] = 'm';
    desc.name[1] = 'o';
    desc.name[2] = 'c';
    desc.name[3] = 'k';
    desc.name[4] = '\0';
    desc.features = input_device_features_none;
    desc.handlers.has_pending = mock_has_pending;
    desc.handlers.read = mock_read;
    desc.type = input_device_type_keyboard;
    desc.state = NULL;
    return desc;
}

static void test_register_mock(test_ctx_t *ctx)
{
    input_device_desc_t desc = make_mock_device();
    int rc = input_device_register(&desc);
    TEST_CHECK_EQ_U(ctx, rc, 0);

    // Clean up so the registration does not leak into the live device list.
    TEST_CHECK_EQ_U(ctx, input_device_unregister(&desc), 0);
}

static void test_unregister_mock(test_ctx_t *ctx)
{
    input_device_desc_t desc = make_mock_device();
    int rc = input_device_register(&desc);
    TEST_CHECK_EQ_U(ctx, rc, 0);

    rc = input_device_unregister(&desc);
    TEST_CHECK_EQ_U(ctx, rc, 0);
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
}
