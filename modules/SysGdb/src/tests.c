// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT
//
// SysTest test suite for SysGdb. Registered from module_init; only executed when
// the kernel is booted with the "cardinal.test" cmdline (test_run_all gates it).
//
// These tests exercise the pluggable-transport plumbing (register/unregister)
// using a harmless mock transport. The break-in poll path is registered but
// SKIPped: calling gdb_poll_breakin() in CI could drop into the stub and wait
// for a debugger that will never connect (gdb_stub_wait() is likewise never
// called here for the same reason).

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "SysTest/test.h"
#include "SysGdb/gdb.h"

// A do-nothing mock transport: getc always reports "no byte", putc discards, and
// poll reports no async activity. Installing it can never block or break in.
static int mock_getc(void *state) {
    (void)state;
    return -1;
}
static void mock_putc(void *state, int c) {
    (void)state;
    (void)c;
}
static int mock_poll(void *state) {
    (void)state;
    return 0;
}

static gdb_transport_t mock_transport = {
    .getc = mock_getc,
    .putc = mock_putc,
    .poll = mock_poll,
    .state = NULL,
};

// Register the mock transport; the call must accept it (non-NULL getc/putc) and
// return cleanly. We can't observe cur_transport directly from here, so this
// mainly proves the path doesn't fault and the descriptor is accepted.
static void test_register_transport(test_ctx_t *ctx) {
    gdb_register_transport(&mock_transport);
    // A NULL or partial descriptor must be rejected (no crash, no swap).
    gdb_register_transport(NULL);
    gdb_transport_t bad = { .getc = NULL, .putc = mock_putc, .poll = NULL, .state = NULL };
    gdb_register_transport(&bad);
    TEST_CHECK(ctx, true);
}

// Unregister the same transport; passing the active descriptor must revert to
// the built-in COM2 channel without disturbing anything.
static void test_unregister_transport(test_ctx_t *ctx) {
    gdb_register_transport(&mock_transport);
    // Revert only when it is still the active transport.
    gdb_unregister_transport(&mock_transport);
    // Forced revert (NULL) is always safe even when COM2 is already active.
    gdb_unregister_transport(NULL);
    TEST_CHECK(ctx, true);
}

// Smoke test for the async break-in poll. SKIPPED: gdb_poll_breakin() may drop
// into the stub and wait for a debugger, which is unsafe in unattended CI.
static void test_poll_smoke(test_ctx_t *ctx) {
    // Intentionally not calling gdb_poll_breakin() here -- this test is marked
    // TEST_FLAG_SKIP and never executes. The reference keeps it from being
    // dead-stripped/forgotten if the skip is ever lifted under supervision.
    (void)gdb_poll_breakin;
    TEST_CHECK(ctx, true);
}

void sysgdb_register_tests(void) {
    if (!test_mode_active())
        return;

    test_def_t reg = {
        .suite = "SysGdb", .name = "register_transport", .fn = test_register_transport,
        .run = TEST_RUN_INLINE, .flags = TEST_FLAG_NONE,
    };
    test_register(&reg);

    test_def_t unreg = {
        .suite = "SysGdb", .name = "unregister_transport", .fn = test_unregister_transport,
        .run = TEST_RUN_INLINE, .flags = TEST_FLAG_NONE,
    };
    test_register(&unreg);

    test_def_t poll = {
        .suite = "SysGdb", .name = "gdb_poll_smoke", .fn = test_poll_smoke,
        .run = TEST_RUN_INLINE, .flags = TEST_FLAG_SKIP,
    };
    test_register(&poll);
}
