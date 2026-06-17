// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT
//
// Death-test support for SysTest. See priv_test.h for the model. This file owns
// the CSMUX control-channel protocol and the trap hooks that turn a fault/PANIC
// during an armed death test into a "DIED" report + reboot.

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <types.h>

#include "elf.h"
#include "SysDebug/csmux.h" // direct: SysDebug loads before SysTest
#include "priv_test.h"

// ---- state ------------------------------------------------------------------

static bool g_harness_mode = false; // "cardinal.harness" in cmdline
static struct {
    volatile bool armed;
    int expect_vector; // expected CPU vector; -1 = any death (fault or PANIC)
    int cursor;
} g_death = {false, -1, 0};

void systest_death_set_harness_mode(bool on) { g_harness_mode = on; }
bool systest_death_harness_mode(void) { return g_harness_mode; }

// Optional wall-clock source (SysTimer), resolved at handshake time. Used to
// bound the handshake by real time rather than by a poll-iteration count: over a
// USB-serial link each idle poll is a full USB round-trip, so an iteration budget
// would translate to a wildly transport-dependent wall-clock timeout.
static uint64_t (*g_now_ns)(void) = NULL;
#ifndef TIMER_NO_COUNTER
#define TIMER_NO_COUNTER ((uint64_t)-1)
#endif

// ---- tiny text builders (no libc printf in kernel space) --------------------

static char *append_str(char *p, char *end, const char *s) {
    while (*s != 0 && p < end - 1)
        *p++ = *s++;
    return p;
}

static char *append_int(char *p, char *end, int64_t v) {
    char tmp[24];
    int i = (int)sizeof(tmp);
    bool neg = v < 0;
    uint64_t u = neg ? (uint64_t)(-v) : (uint64_t)v;
    tmp[--i] = '\0';
    if (u == 0)
        tmp[--i] = '0';
    while (u > 0 && i > 0) {
        tmp[--i] = (char)('0' + (u % 10));
        u /= 10;
    }
    if (neg && i > 0)
        tmp[--i] = '-';
    return append_str(p, end, &tmp[i]);
}

void systest_ctrl_send(const char *msg) {
    char line[256];
    char *p = line;
    char *end = line + sizeof(line);
    p = append_str(p, end, msg);
    p = append_str(p, end, "\n");
    csmux_send(CSMUX_CH_CTRL, line, (uint32_t)(p - line));
}

// Send "<tag_eq><value>" (e.g. systest_ctrl_kv("SURVIVED cursor=", 3)).
void systest_ctrl_kv(const char *tag_eq, int value) {
    char line[128];
    char *p = line;
    char *e = line + sizeof(line);
    p = append_str(p, e, tag_eq);
    p = append_int(p, e, value);
    *p = '\0';
    systest_ctrl_send(line);
}

// Send "BEGIN cursor=C suite=S name=N expect=V" for the death test about to run.
void systest_ctrl_begin(int cursor, const char *suite, const char *name, int expect) {
    char line[256];
    char *p = line;
    char *e = line + sizeof(line);
    p = append_str(p, e, "BEGIN cursor=");
    p = append_int(p, e, cursor);
    p = append_str(p, e, " suite=");
    p = append_str(p, e, suite);
    p = append_str(p, e, " name=");
    p = append_str(p, e, name);
    p = append_str(p, e, " expect=");
    p = append_int(p, e, expect);
    *p = '\0';
    systest_ctrl_send(line);
}

// ---- control-channel receive (bounded) --------------------------------------

// Read one '\n'-terminated line from CH_CTRL into buf. Returns length (excl.
// NUL) or -1 on timeout. Bounded by `timeout_ms` of wall-clock when a timer is
// available, else by `spin_budget` consecutive empty polls -- either way this can
// never hang the run if no harness is listening.
static int ctrl_read_line(char *buf, uint32_t cap, uint32_t spin_budget, uint32_t timeout_ms) {
    uint32_t pos = 0;
    uint32_t idle = 0;
    uint64_t deadline = 0;
    bool have_clock = false;
    if (g_now_ns != NULL) {
        uint64_t now = g_now_ns();
        if (now != TIMER_NO_COUNTER) {
            have_clock = true;
            deadline = now + (uint64_t)timeout_ms * 1000000ull;
        }
    }
    for (;;) {
        char c;
        int n = csmux_chan_read(CSMUX_CH_CTRL, &c, 1);
        if (n == 1) {
            idle = 0;
            if (c == '\r')
                continue;
            if (c == '\n') {
                buf[pos] = '\0';
                return (int)pos;
            }
            if (pos < cap - 1)
                buf[pos++] = c;
            continue;
        }
        if (have_clock) {
            if (g_now_ns() >= deadline)
                return -1;
        } else if (++idle >= spin_budget) {
            return -1;
        }
        for (volatile int d = 0; d < 64; d++)
            ; // brief pause between polls
    }
}

// Parse the integer following `key` (e.g. "cursor=") in `s`; -1 if not found.
static int parse_int_after(const char *s, const char *key) {
    const char *at = strstr(s, key);
    if (at == NULL)
        return -1;
    at += strlen(key);
    bool neg = false;
    if (*at == '-') {
        neg = true;
        at++;
    }
    if (*at < '0' || *at > '9')
        return -1;
    int v = 0;
    while (*at >= '0' && *at <= '9')
        v = v * 10 + (*at++ - '0');
    return neg ? -v : v;
}

// Give USB enumeration a chance to bind a "heavy" transport (an FTDI USB-serial
// adapter) before the handshake, so the mux rides that single link rather than
// COM1. Bounded by `timeout_ms`; returns immediately once a heavy transport
// appears, and is a harmless short wait when none will (COM1-only runs).
static void wait_for_heavy_transport(uint32_t timeout_ms) {
    if (csmux_xport_heavy())
        return;
    uint64_t deadline = 0;
    bool have_clock = (g_now_ns != NULL) && (g_now_ns() != TIMER_NO_COUNTER);
    if (have_clock)
        deadline = g_now_ns() + (uint64_t)timeout_ms * 1000000ull;
    uint32_t spins = 0;
    while (!csmux_xport_heavy()) {
        if (have_clock) {
            if (g_now_ns() >= deadline)
                return;
        } else if (++spins >= 2000000u) {
            return;
        }
        for (volatile int d = 0; d < 256; d++)
            ;
    }
}

bool systest_death_handshake(int death_count, int *cursor) {
    // Resolve a wall-clock source so the handshake timeout is real-time bounded
    // regardless of the link (COM1 polls are cheap; FTDI polls are USB round-trips).
    if (g_now_ns == NULL)
        g_now_ns = (uint64_t (*)(void))elf_resolvefunction("timer_timestamp_ns");

    // Let an FTDI link enumerate so we mux over it instead of COM1 (real HW path).
    // Bounded; harmless when no USB-serial is present (COM1-only runs).
    wait_for_heavy_transport(3000 /* ms */);

    csmux_activate();

    char hello[64];
    char *p = hello;
    char *end = hello + sizeof(hello);
    p = append_str(p, end, "HELLO proto=1 deaths=");
    p = append_int(p, end, death_count);
    *p = '\0';
    systest_ctrl_send(hello);

    char line[256];
    int n = ctrl_read_line(line, sizeof(line), 4000000, 4000 /* ms */);
    if (n < 0)
        return false;
    if (strncmp(line, "OLEH", 4) != 0)
        return false;
    int c = parse_int_after(line, "cursor=");
    if (cursor != NULL)
        *cursor = c < 0 ? 0 : c;
    return true;
}

// ---- arm / report / hooks ---------------------------------------------------

void systest_death_arm(int expect_vector, int cursor) {
    g_death.expect_vector = expect_vector;
    g_death.cursor = cursor;
    g_death.armed = true;
}
void systest_death_disarm(void) { g_death.armed = false; }
bool systest_death_active(void) { return g_death.armed; }

// Common death path: report the outcome on CH_CTRL and reboot. Returns normally
// (so the caller's normal panic/shell path proceeds) ONLY when not armed.
static void death_report_and_reset(int vector) {
    if (!g_death.armed)
        return; // not a death test -> let the normal panic/shell run
    g_death.armed = false;

    char msg[64];
    char *p = msg;
    char *e = msg + sizeof(msg);
    p = append_str(p, e, "DIED cursor=");
    p = append_int(p, e, g_death.cursor);
    p = append_str(p, e, " vec=");
    p = append_int(p, e, vector);
    *p = '\0';
    systest_ctrl_send(msg);

    system_reset(); // never returns
}

// Installed into SysInterrupts (unhandled CPU exception) and SysDebug (PANIC).
static void death_oncpufault(int vector) { death_report_and_reset(vector); }
static void death_onpanic(void) { death_report_and_reset(-1); }

void systest_death_install_hooks(void) {
    // SysDebug is loaded before SysTest -> direct call.
    debug_set_trap_hook(death_onpanic);
    // SysInterrupts is loaded after SysTest -> resolve at runtime.
    void (*set_death_hook)(void (*)(int)) =
        (void (*)(void (*)(int)))elf_resolvefunction("interrupt_set_death_hook");
    if (set_death_hook != NULL)
        set_death_hook(death_oncpufault);
}
