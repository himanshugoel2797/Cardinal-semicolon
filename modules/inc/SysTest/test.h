// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINAL_SYSTEST_H
#define CARDINAL_SYSTEST_H

#include <stdint.h>
#include <stdbool.h>
#include <cardinal/cs_error.h>

// SysTest -- an in-OS unit test framework.
//
// Any module/server/driver can register tests during its module_init (SysTest
// is always loaded, so test_register always resolves -- only *execution* is
// gated by the kernel cmdline). When the kernel is booted with the "cardinal.test"
// cmdline flag, test_run_all() (called from servicescript.txt once the whole
// service stack is up) runs every registered test, prints TAP-style results over
// the debug serial, and exits the machine with a pass/fail code. On a normal boot
// test_run_all() is a cheap no-op, so registering tests has no runtime cost.

// Opaque per-test result accumulator. The assertion macros below operate on it.
typedef struct test_ctx test_ctx_t;

typedef void (*test_fn_t)(test_ctx_t *ctx);

// When/how a test runs. All triggers fire from the post-boot sweep in
// test_run_all() (or an explicit test_run_suite()); the difference is execution
// context.
typedef enum {
    // Run synchronously, inline, in the test runner's own thread. The default;
    // right for pure logic / data-structure tests that never block.
    TEST_RUN_INLINE = 0,
    // Run in a dedicated kernel task. Use when the test must sleep, yield, or
    // otherwise block (anything touching the scheduler / task APIs).
    TEST_RUN_TASK,
    // Run once on every online core, each on its own pinned kernel task. Use for
    // SMP / per-core-state correctness. The test sees its core via test_core().
    TEST_RUN_PERCPU,
} test_run_t;

typedef struct {
    const char *suite;   // group name, e.g. "SysMemory"
    const char *name;    // test name, e.g. "realloc_grows"
    test_fn_t   fn;
    test_run_t  run;     // execution context (default TEST_RUN_INLINE == 0)
    uint32_t    flags;   // see TEST_FLAG_*
    // Death tests only (TEST_FLAG_DEATH). The CPU exception vector the lethal op
    // is expected to raise (e.g. 14 for #PF, 13 for #GP, 6 for #UD), or -1 for
    // "any death" (any fault, or a PANIC). Ignored unless TEST_FLAG_DEATH is set.
    // Trailing field: existing (designated/partial) initializers stay valid and
    // zero-fill it -- so set it explicitly (or use TEST_DEATH) for death tests.
    int32_t     expect_vector;
} test_def_t;

#define TEST_FLAG_NONE 0u
#define TEST_FLAG_SKIP (1u << 0) // registered but not executed (reported as skipped)
// A death test: fn is expected to KILL the kernel (fault/PANIC). It runs only
// under a harness-driven local run (see priv_test.h); in plain test mode it is
// reported skipped. If fn returns without dying, the test FAILS ("survived").
#define TEST_FLAG_DEATH (1u << 1)

// Expected-fault sentinel for "any death is acceptable".
#define TEST_DEATH_ANY (-1)

// Convenience registrar for a death test. `vec` is the expected CPU vector
// (or TEST_DEATH_ANY). Death tests run inline (they are not meant to return).
#define TEST_DEATH_DEF(suite_, name_, fn_, vec_)                       \
    ((test_def_t){.suite = (suite_), .name = (name_), .fn = (fn_),     \
                  .run = TEST_RUN_INLINE, .flags = TEST_FLAG_DEATH,    \
                  .expect_vector = (vec_)})

// Register a test. Safe to call from any module_init. The def is copied, so a
// stack/temporary is fine. Returns CS_OK, or CS_OUTOFMEM / CS_INVALIDARG.
cs_error test_register(const test_def_t *def);

// True iff the kernel was booted in test mode ("cardinal.test" in the cmdline).
// Cheap; usable as a guard around expensive or destructive test-only setup.
bool test_mode_active(void);

// Run every registered test, report, and (in test mode) exit the machine.
// Invoked from servicescript.txt via CALL:test_run_all. No-op outside test mode.
// Returns 0 (it does not return at all when it exits the machine).
int test_run_all(void);

// Run just the named suite synchronously, in the caller's thread, and report it.
// Lets a module trigger its own tests at a chosen point (e.g. right after it
// finishes init) rather than waiting for the global post-boot sweep. Tests run
// this way are marked done and skipped by the later test_run_all() sweep.
// No-op outside test mode. Returns the number of failed checks in the suite.
int test_run_suite(const char *suite);

// --- For the current core index inside a TEST_RUN_PERCPU test (0 otherwise). ---
int test_core(const test_ctx_t *ctx);

// --- Assertion primitives. Record into ctx; never abort the run. ---
// Implemented in SysTest; declared here so any module's test sources can use them.
void test_report_fail(test_ctx_t *ctx, const char *file, int line, const char *expr);
void test_report_check(test_ctx_t *ctx);

#define TEST_CHECK_MSG(ctx, cond, msg)                       \
    do {                                                     \
        test_report_check((ctx));                            \
        if (!(cond))                                         \
            test_report_fail((ctx), __FILE__, __LINE__, msg);\
    } while (0)

#define TEST_CHECK(ctx, cond) TEST_CHECK_MSG(ctx, (cond), #cond)

#define TEST_CHECK_EQ_U(ctx, a, b) \
    TEST_CHECK_MSG(ctx, (uint64_t)(a) == (uint64_t)(b), #a " == " #b)

#define TEST_CHECK_NE_U(ctx, a, b) \
    TEST_CHECK_MSG(ctx, (uint64_t)(a) != (uint64_t)(b), #a " != " #b)

#define TEST_CHECK_EQ_PTR(ctx, a, b) \
    TEST_CHECK_MSG(ctx, (void *)(a) == (void *)(b), #a " == " #b)

#define TEST_FAIL(ctx, msg) TEST_CHECK_MSG(ctx, 0, msg)

#endif
