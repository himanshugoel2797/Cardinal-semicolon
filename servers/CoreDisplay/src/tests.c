// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#include <string.h>

#include "SysTest/test.h"
#include "CoreDisplay/display.h"

// Build a minimal mock display descriptor: known name, unknown connection,
// zeroed handlers, no state.
static void make_mock_desc(display_desc_t *desc)
{
    memset(desc, 0, sizeof(*desc));
    strncpy(desc->display_name, "test", sizeof(desc->display_name) - 1);
    desc->connection = display_connection_unkn;
    desc->state = NULL;
}

// display_register of a minimal valid descriptor succeeds.
static void test_register_minimal(test_ctx_t *ctx)
{
    display_desc_t desc;
    make_mock_desc(&desc);

    int rc = display_register(&desc);
    TEST_CHECK_EQ_U(ctx, rc, 0);

    // Clean up so the test leaves no entry behind.
    display_unregister(&desc);
}

// display_unregister of a previously-registered descriptor succeeds.
static void test_unregister_minimal(test_ctx_t *ctx)
{
    display_desc_t desc;
    make_mock_desc(&desc);

    int rc = display_register(&desc);
    TEST_CHECK_EQ_U(ctx, rc, 0);

    rc = display_unregister(&desc);
    TEST_CHECK_EQ_U(ctx, rc, 0);
}

void coredisplay_register_tests(void)
{
    if (!test_mode_active())
        return;

    {
        test_def_t t = {
            .suite = "CoreDisplay",
            .name = "register_minimal",
            .fn = test_register_minimal,
            .run = TEST_RUN_INLINE,
            .flags = TEST_FLAG_NONE,
        };
        test_register(&t);
    }

    {
        test_def_t t = {
            .suite = "CoreDisplay",
            .name = "unregister_minimal",
            .fn = test_unregister_minimal,
            .run = TEST_RUN_INLINE,
            .flags = TEST_FLAG_NONE,
        };
        test_register(&t);
    }
}
