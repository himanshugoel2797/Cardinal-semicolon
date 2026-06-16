// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINAL_SYSTEST_PRIV_H
#define CARDINAL_SYSTEST_PRIV_H

#include <stdint.h>
#include <stdbool.h>

#include "SysTest/test.h"

// Per-test result accumulator. test_ctx_t is opaque to test authors (test.h);
// the framework owns the layout.
struct test_ctx {
    const char *suite;
    const char *name;
    int core;        // core index for a TEST_RUN_PERCPU invocation, else 0
    uint32_t checks; // assertions evaluated
    uint32_t fails;  // assertions failed
};

// A registered test plus its accumulated result.
typedef struct {
    test_def_t def;
    bool done;       // already executed (via test_run_suite) -- skip in the sweep
    uint32_t checks; // aggregated across all invocations (>1 for PERCPU)
    uint32_t fails;
} test_entry_t;

// Terminate the machine after the test run. code 0 == all passed. On QEMU with
// `-device isa-debug-exit,iobase=0xf4,...` this exits the emulator promptly;
// otherwise it falls back to halting the CPU. Never returns.
void test_platform_exit(int code) NORETURN;

#endif
