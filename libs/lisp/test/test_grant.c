// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// Host test for the shared-memory grant substrate (grant.c) -- the compositor's
// zero-copy surface capability (notes/servers/CoreCompositor.md). The grant TABLE
// (mint/resolve/revoke + generation invalidation) is portable and exercised here
// directly against the C API. The actual phys->virt mapping (map-grant) is
// kernel-only (vmem) and is covered by the in-OS self-test instead; what this adds
// over that is the identity-on-send guarantee, which needs a running scheduler.

#include <stdio.h>
#include <string.h>

#include "lisp.h"

static int failures = 0;
static int checks = 0;

static void chk(const char *what, int cond) {
    checks++;
    if (!cond) {
        printf("  FAIL %s\n", what);
        failures++;
    } else {
        printf("  ok   %s\n", what);
    }
}

// Render a value to a small buffer for string comparison (the send test).
static const char *render(lisp_value v, char *buf, size_t cap) {
    lisp_print(v, buf, cap);
    return buf;
}

int main(void) {
    uintptr_t stack_marker;
    lisp_gc_init(&stack_marker);
    lisp_default_env();  // builds the system heap / interns; grants live in it

    printf("[lisp grant] mint / resolve / revoke / generation / send-identity\n");

    uint64_t phys;
    size_t len;
    uint32_t perms;

    // 1. mint -> a grant that resolves to the region it was minted over.
    lisp_value g = lisp_grant_mint(0x100000, 4096, 1);
    chk("mint returns a grant", lisp_is_grant(g));
    chk("fresh grant resolves", lisp_grant_resolve(g, &phys, &len, &perms) == 0);
    chk("resolved region matches mint", phys == 0x100000 && len == 4096 && perms == 1);

    // 2. revoke -> resolve fails; revoke is idempotent.
    chk("revoke succeeds", lisp_grant_revoke(g) == 0);
    chk("revoked grant no longer resolves", lisp_grant_resolve(g, &phys, &len, &perms) == -1);
    chk("revoke is idempotent (already dead -> -1)", lisp_grant_revoke(g) == -1);

    // 3. read-only perms round-trip.
    lisp_value gro = lisp_grant_mint(0x200000, 8192, 0);
    chk("ro grant resolves with perms=0",
        lisp_grant_resolve(gro, &phys, &len, &perms) == 0 && perms == 0);

    // 4. generation: revoking then re-minting reuses the slot and bumps its
    //    generation, so the STALE handle to the reused slot must not resolve --
    //    the core defense against a revoked grant mapping a new occupant.
    lisp_grant_revoke(gro);
    lisp_value gnew = lisp_grant_mint(0x300000, 4096, 1);
    chk("re-mint resolves to the new region",
        lisp_grant_resolve(gnew, &phys, &len, &perms) == 0 && phys == 0x300000);
    chk("stale handle to a reused slot does NOT resolve",
        lisp_grant_resolve(gro, &phys, &len, &perms) == -1);

    // 5. resolve/revoke reject non-grant values (a forged integer cannot resolve).
    chk("resolve rejects a fixnum", lisp_grant_resolve(lisp_fixnum(5), &phys, &len, &perms) == -1);
    chk("revoke rejects a string", lisp_grant_revoke(lisp_make_string("x", 1)) == -1);

    // 6. identity-on-send: a grant is passed BY IDENTITY across a (send), not
    //    deep-copied and not rejected as non-data. The consumer compares the
    //    received value to the original binding with eq? -> #t proves identity.
    {
        lisp_sched_t s;
        lisp_sched_init(&s, 64);
        lisp_value env = lisp_default_env();
        lisp_install_sched(env);
        const char *err = NULL;
        char buf[64];

        lisp_env_define(env, lisp_make_symbol("g6", 2), lisp_grant_mint(0x400000, 4096, 1));
        lisp_eval_string(
            "(define consumer (spawn (lambda () (eq? (recv) g6))))"
            "(define producer (spawn (lambda () (send consumer g6))))",
            env, &err);
        if (err)
            printf("  (send setup error: %s)\n", err);
        lisp_value consumer = lisp_eval_string("consumer", env, &err);
        lisp_sched_run(&s, 0);
        chk("grant survives (send) by identity (eq? -> #t)",
            strcmp(render(lisp_ctx_value(consumer), buf, sizeof buf), "#t") == 0);
    }

    // 7. read-only enforcement: a bytes view marked read-only (as map-grant marks
    //    a 'ro grant) refuses writes through every mutator but still reads. Here we
    //    mark a heap-backed foreign view directly, since map-grant itself is
    //    kernel-only. The guard fires before touching memory.
    {
        static uint8_t backing[64];
        lisp_value ro = lisp_make_bytes_foreign(backing, sizeof backing, 0x9000);
        lisp_bytes_mark_readonly(ro);
        chk("readonly flag is observable", lisp_bytes_readonly(ro));

        lisp_value env = lisp_default_env();
        lisp_install_sched(env);  // env may host scheduler prims; harmless here
        lisp_env_define(env, lisp_make_symbol("ro", 2), ro);
        const char *err = NULL;

        lisp_eval_string("(bytes-u32-set! ro 0 5)", env, &err);
        chk("RO refuses bytes-u32-set!", err != NULL);
        err = NULL;
        lisp_eval_string("(bytes-fill32! ro 0 1 5)", env, &err);
        chk("RO refuses bytes-fill32!", err != NULL);
        err = NULL;
        lisp_eval_string("(gfx-fill-rect! ro 4 1 1 0 0 1 1 255)", env, &err);
        chk("RO refuses gfx-fill-rect!", err != NULL);
        err = NULL;
        lisp_eval_string("(bytes-u8-ref ro 0)", env, &err);  // reads stay allowed
        chk("RO still allows reads", err == NULL);
    }

    // 8. table exhaustion is a hard error, not a silent overwrite. Done last
    //    because it consumes the rest of the table for this process.
    int minted = 0;
    while (lisp_grant_mint(0x500000, 4096, 1) != LISP_UNDEF)
        minted++;
    chk("grant table eventually reports full (no silent overwrite)", minted > 0);

    printf("[lisp grant] %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
