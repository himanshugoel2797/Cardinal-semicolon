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

// display_unregister of a previously-registered descriptor succeeds and is
// observable in two ways:
//   (a) a second unregister of the same descriptor returns 1 (not found),
//       proving the entry was actually removed from the list;
//   (b) re-registering the same descriptor pointer succeeds (returns 0),
//       proving it is no longer in the list and can be re-added.
// The test cleans up by unregistering the re-registered entry.
static void test_unregister_minimal(test_ctx_t *ctx)
{
    display_desc_t desc;
    make_mock_desc(&desc);

    int rc = display_register(&desc);
    TEST_CHECK_EQ_U(ctx, rc, 0);

    rc = display_unregister(&desc);
    TEST_CHECK_EQ_U(ctx, rc, 0);

    // (a) Second unregister must return 1 (not found) — entry is gone.
    int rc2 = display_unregister(&desc);
    TEST_CHECK_EQ_U(ctx, rc2, 1);

    // (b) Re-register must succeed — no stale entry blocks insertion.
    int rc3 = display_register(&desc);
    TEST_CHECK_EQ_U(ctx, rc3, 0);

    // Clean up.
    display_unregister(&desc);
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
