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

// Reboot the machine (0xCF9, then 8042, then triple fault). Used by the death
// test path so the harness can advance the death cursor on the next boot. Never
// returns.
void system_reset(void) NORETURN;

// ---- death-test support (death.c) ------------------------------------------
//
// A death test asserts that some operation *kills* the kernel (an expected CPU
// fault, or a PANIC). The verdict cannot survive in-guest, so a host harness on
// the CSMUX control channel (CSMUX_CH_CTRL) holds a death-test cursor across
// reboots: each boot runs the death test at the cursor, which dies and reboots;
// the harness then advances the cursor and the next boot continues. Death tests
// only run when the harness handshake succeeds (local runs); otherwise they are
// reported skipped. All of this is gated on the "cardinal.harness" cmdline token
// in addition to "cardinal.test".

// Set from module_init: true iff "cardinal.harness" is in the kernel cmdline.
void systest_death_set_harness_mode(bool on);
bool systest_death_harness_mode(void);

// Send "HELLO proto=1 deaths=<K>" on CH_CTRL and wait (bounded) for the harness
// reply "OLEH cursor=<c>". Returns true and writes *cursor on success; false on
// timeout (no harness present). Activates CSMUX before sending.
bool systest_death_handshake(int death_count, int *cursor);

// Install the CPU-fault and PANIC death hooks (idempotent). Call once before
// arming a death test on this boot.
void systest_death_install_hooks(void);

// Arm/disarm the death context. While armed, an unhandled CPU exception or a
// PANIC reports "DIED" on CH_CTRL and reboots instead of panicking.
void systest_death_arm(int expect_vector, int cursor);
void systest_death_disarm(void);
bool systest_death_active(void);

// Send a one-line text message on the CSMUX control channel (newline appended).
void systest_ctrl_send(const char *msg);
// Send "<tag_eq><value>" e.g. systest_ctrl_kv("SURVIVED cursor=", 3).
void systest_ctrl_kv(const char *tag_eq, int value);
// Send "BEGIN cursor=C suite=S name=N expect=V".
void systest_ctrl_begin(int cursor, const char *suite, const char *name, int expect);

#endif
