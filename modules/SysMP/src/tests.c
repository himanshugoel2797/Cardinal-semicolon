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

// INLINE: a TLS slot allocation must succeed (returns a non-negative offset).
static void test_tls_alloc(test_ctx_t *ctx) {
    int off = mp_tls_alloc(sizeof(uintptr_t));
    TEST_CHECK(ctx, off >= 0);
}

// PERCPU: the shared TLS slot must resolve to non-NULL storage on every online
// core, proving per-core TLS is wired up on each core.
static void test_tls_percpu(test_ctx_t *ctx) {
    TEST_CHECK_MSG(ctx, s_tls_off >= 0, "TLS slot was not allocated");
    if (s_tls_off < 0)
        return;
    TEST_CHECK_MSG(ctx, mp_tls_get(s_tls_off) != NULL, "per-core TLS storage is NULL");
}

void sysmp_register_tests(void) {
    if (!test_mode_active())
        return;

    // Allocate one TLS slot up front; the per-core test reads it on each core.
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
