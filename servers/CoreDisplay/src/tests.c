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

// --- mock handler contract ---------------------------------------------------
//
// CoreDisplay today exposes no public function that walks the registered-display
// list and calls back into a driver's display_handlers_t -- registration just
// records the descriptor (the lfb driver is the only consumer and CoreDisplay
// never dispatches through it). So the "handler dispatch path" a driver fulfils
// is: CoreDisplay preserves the handler table verbatim through register(), and a
// caller invokes the handlers via desc->handlers, passing desc->state. These
// mocks record that they were called and with which state/args, letting the test
// drive that contract and prove the table survives registration intact.
static void *mock_seen_state;

static display_res_info_t mock_seen_res;
static int mock_set_resolution(void *state, display_res_info_t *info)
{
    mock_seen_state = state;
    mock_seen_res = *info;
    return 0;
}

static uintptr_t mock_fb_addr = 0xCAFEB000;
static int mock_get_framebuffer(void *state, uintptr_t *addr)
{
    mock_seen_state = state;
    *addr = mock_fb_addr;
    return 0;
}

static int mock_get_status(void *state, display_status_t *ans)
{
    mock_seen_state = state;
    *ans = display_status_connected;
    return 0;
}

static int mock_flush_called;
static int mock_flush(void *state)
{
    mock_seen_state = state;
    mock_flush_called = 1;
    return 0;
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

// display_register(NULL) / display_unregister(NULL) must both reject with -1,
// and unregistering a descriptor that was never registered must return 1
// (not-found) rather than spuriously succeeding.
static void test_null_and_unknown_guards(test_ctx_t *ctx)
{
    TEST_CHECK_EQ_U(ctx, display_register(NULL), -1);
    TEST_CHECK_EQ_U(ctx, display_unregister(NULL), -1);

    // A descriptor that was never registered: unregister returns 1 (not found).
    display_desc_t never;
    make_mock_desc(&never);
    TEST_CHECK_EQ_U(ctx, display_unregister(&never), 1);
}

// Register a descriptor whose handler table is wired to the mocks above, then
// drive each handler the way a caller would (through desc.handlers, with
// desc.state) and assert the mock saw the right state and arguments. This proves
// register() preserves the display_handlers_t contract a driver fulfils.
static void test_handler_dispatch(test_ctx_t *ctx)
{
    int sentinel_state = 0;

    display_desc_t desc;
    make_mock_desc(&desc);
    desc.state = &sentinel_state;
    desc.handlers.set_resolution = mock_set_resolution;
    desc.handlers.get_framebuffer = mock_get_framebuffer;
    desc.handlers.get_status = mock_get_status;
    desc.handlers.flush = mock_flush;

    TEST_CHECK_EQ_U(ctx, display_register(&desc), 0);

    // The handler table must survive registration unchanged.
    TEST_CHECK_EQ_PTR(ctx, desc.handlers.set_resolution, mock_set_resolution);
    TEST_CHECK_EQ_PTR(ctx, desc.handlers.get_framebuffer, mock_get_framebuffer);
    TEST_CHECK_EQ_PTR(ctx, desc.handlers.get_status, mock_get_status);
    TEST_CHECK_EQ_PTR(ctx, desc.handlers.flush, mock_flush);

    // set_resolution: mock receives the descriptor's state and the exact res.
    display_res_info_t want = {
        .w_res = 1920, .h_res = 1080, .stride = 1920 * 4, .refresh_rate = 60,
    };
    mock_seen_state = NULL;
    int rc = desc.handlers.set_resolution(desc.state, &want);
    TEST_CHECK_EQ_U(ctx, rc, 0);
    TEST_CHECK_EQ_PTR(ctx, mock_seen_state, desc.state);
    TEST_CHECK_EQ_U(ctx, mock_seen_res.w_res, 1920);
    TEST_CHECK_EQ_U(ctx, mock_seen_res.h_res, 1080);
    TEST_CHECK_EQ_U(ctx, mock_seen_res.refresh_rate, 60);

    // get_framebuffer: mock returns its sentinel address via the out-param.
    uintptr_t addr = 0;
    mock_seen_state = NULL;
    rc = desc.handlers.get_framebuffer(desc.state, &addr);
    TEST_CHECK_EQ_U(ctx, rc, 0);
    TEST_CHECK_EQ_PTR(ctx, mock_seen_state, desc.state);
    TEST_CHECK_EQ_U(ctx, addr, mock_fb_addr);

    // get_status: mock reports "connected" via the out-param.
    display_status_t st = display_status_disconnected;
    mock_seen_state = NULL;
    rc = desc.handlers.get_status(desc.state, &st);
    TEST_CHECK_EQ_U(ctx, rc, 0);
    TEST_CHECK_EQ_PTR(ctx, mock_seen_state, desc.state);
    TEST_CHECK_EQ_U(ctx, st, display_status_connected);

    // flush: mock records it ran with the descriptor's state.
    mock_flush_called = 0;
    mock_seen_state = NULL;
    rc = desc.handlers.flush(desc.state);
    TEST_CHECK_EQ_U(ctx, rc, 0);
    TEST_CHECK_EQ_U(ctx, mock_flush_called, 1);
    TEST_CHECK_EQ_PTR(ctx, mock_seen_state, desc.state);

    // Clean up so the test leaves no entry behind.
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

    {
        test_def_t t = {
            .suite = "CoreDisplay",
            .name = "null_and_unknown_guards",
            .fn = test_null_and_unknown_guards,
            .run = TEST_RUN_INLINE,
            .flags = TEST_FLAG_NONE,
        };
        test_register(&t);
    }

    {
        test_def_t t = {
            .suite = "CoreDisplay",
            .name = "handler_dispatch",
            .fn = test_handler_dispatch,
            .run = TEST_RUN_INLINE,
            .flags = TEST_FLAG_NONE,
        };
        test_register(&t);
    }
}
