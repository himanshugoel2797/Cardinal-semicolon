// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT
//
// SysTest tests for SysMP: sanity-check the core count, the platform CPU-state
// size, TLS-slot allocation, and that per-core TLS storage resolves on each
// online core.

#include <stddef.h>
#include <stdint.h>
#include <types.h>

#include "SysMP/mp.h"
#include "SysTest/test.h"

// A TLS slot allocated once at registration time and shared by the per-core
// test below. mp_tls_alloc returns a byte offset into each core's TLS block.
static int s_tls_off = -1;

// INLINE: at least one core must be online.
static void test_corecount(test_ctx_t *ctx) {
    TEST_CHECK(ctx, mp_corecount() >= 1);
}

// INLINE: the platform CPU-state blob must be non-empty.
static void test_statesize(test_ctx_t *ctx) {
    TEST_CHECK(ctx, mp_platform_getstatesize() > 0);
}

// INLINE: mp_tls_alloc bumps a shared cursor, so two successive allocations
// must hand out distinct, non-overlapping offsets that advance by at least the
// requested size. A bare "off >= 0" check is vacuous -- the only failure path
// inside mp_tls_alloc calls PANIC() and never returns.
static void test_tls_alloc(test_ctx_t *ctx) {
    int a = mp_tls_alloc(sizeof(uintptr_t));
    int b = mp_tls_alloc(sizeof(uintptr_t));
    TEST_CHECK(ctx, a >= 0);
    TEST_CHECK_MSG(ctx, b >= a + (int)sizeof(uintptr_t),
                   "second allocation must advance past the first");
}

// PERCPU: prove per-core TLS storage is actually backed by memory on every
// online core. mp_tls_get returns a GS-relative (address_space 256) pointer;
// the previous test only compared that pointer to NULL (i.e. checked the offset
// was non-zero) and never touched the storage. Here we dereference it -- a real
// %gs-relative store and load -- and verify the slot retains what we wrote. An
// uninitialised GS base or an unmapped slot would fault or read back garbage.
static void test_tls_percpu(test_ctx_t *ctx) {
    TEST_CHECK_MSG(ctx, s_tls_off > 0, "TLS slot was not allocated");
    if (s_tls_off <= 0)
        return;

    TLS uintptr_t *slot = (TLS uintptr_t *)mp_tls_get(s_tls_off);
    TEST_CHECK_MSG(ctx, slot != NULL, "per-core TLS storage is NULL");
    if (slot == NULL)
        return;

    const uintptr_t sentinel = (uintptr_t)0xC0FFEE00u ^ (uintptr_t)s_tls_off;
    *slot = sentinel;
    TEST_CHECK_MSG(ctx, *slot == sentinel,
                   "per-core TLS slot did not retain the written value");
}

void sysmp_register_tests(void) {
    if (!test_mode_active())
        return;

    // Allocate one TLS slot up front; the per-core test reads it on each core.
    // mp_tls_get(0) returns the address_space(256) null pointer, so if we land
    // at offset 0 (SysMP is loaded early) burn it and grab the next slot.
    s_tls_off = mp_tls_alloc(sizeof(uintptr_t));
    if (s_tls_off == 0)
        s_tls_off = mp_tls_alloc(sizeof(uintptr_t));

    test_def_t corecount = {
        .suite = "SysMP", .name = "corecount_ge1", .fn = test_corecount,
        .run = TEST_RUN_INLINE, .flags = TEST_FLAG_NONE,
    };
    test_register(&corecount);

    test_def_t statesize = {
        .suite = "SysMP", .name = "statesize_positive", .fn = test_statesize,
        .run = TEST_RUN_INLINE, .flags = TEST_FLAG_NONE,
    };
    test_register(&statesize);

    test_def_t tls_alloc = {
        .suite = "SysMP", .name = "tls_alloc", .fn = test_tls_alloc,
        .run = TEST_RUN_INLINE, .flags = TEST_FLAG_NONE,
    };
    test_register(&tls_alloc);

    test_def_t tls_percpu = {
        .suite = "SysMP", .name = "tls_get_percpu", .fn = test_tls_percpu,
        .run = TEST_RUN_PERCPU, .flags = TEST_FLAG_NONE,
    };
    test_register(&tls_percpu);
}
