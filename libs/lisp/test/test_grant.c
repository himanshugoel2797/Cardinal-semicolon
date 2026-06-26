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

    // 8. use-after-revoke = zero page. A granted view re-validates against the grant
    //    table on every access (it carries the grant's index+generation, as map-grant
    //    stamps it). While live it reads/writes its backing; once the grant is REVOKED
    //    the view reads as zeros and refuses writes, so a late access can neither read
    //    the (reused) RAM nor corrupt it. (Stamped directly here -- map-grant is
    //    kernel-only.) This is the use-after-revoke hardening; see notes/AUDIT.md.
    {
        static uint8_t backing[64];
        memset(backing, 0xAB, sizeof backing);
        lisp_value g8 = lisp_grant_mint(0x600000, sizeof backing, 1);  // rw
        uint32_t gi = 0, gg = 0;
        lisp_grant_handle(g8, &gi, &gg);
        lisp_value view = lisp_make_bytes_foreign(backing, sizeof backing, 0x600000);
        lisp_bytes_set_grant(view, gi, gg);

        chk("grant_handle reads a non-zero generation", gg != 0);
        chk("live grant: is_live true", lisp_grant_is_live(gi, gg) == 1);
        chk("live grant: view not dead", !lisp_bytes_grant_dead(view));

        lisp_value env = lisp_default_env();
        lisp_install_sched(env);
        lisp_env_define(env, lisp_make_symbol("gv", 2), view);
        const char *err = NULL;

        // live: reads see the backing, writes land.
        lisp_value r = lisp_eval_string("(bytes-u8-ref gv 0)", env, &err);
        chk("live grant: read sees backing (0xAB)", err == NULL && lisp_fixnum_val(r) == 0xAB);
        err = NULL;
        lisp_eval_string("(bytes-u8-set! gv 0 1)", env, &err);
        chk("live grant: write lands", err == NULL && backing[0] == 1);

        // revoke -> the view goes dead.
        lisp_grant_revoke(g8);
        chk("revoked: is_live false", lisp_grant_is_live(gi, gg) == 0);
        chk("revoked: view reports dead", lisp_bytes_grant_dead(view));

        backing[0] = 0xCD;  // pretend the freed RAM was reused with new content
        err = NULL;
        r = lisp_eval_string("(bytes-u8-ref gv 0)", env, &err);
        chk("revoked: read returns 0, not the reused RAM", err == NULL && lisp_fixnum_val(r) == 0);
        err = NULL;
        lisp_eval_string("(bytes-u8-set! gv 0 9)", env, &err);
        chk("revoked: write refused", err != NULL);
        chk("revoked: refused write left backing untouched", backing[0] == 0xCD);
        err = NULL;
        lisp_eval_string("(gfx-fill-rect! gv 4 1 1 0 0 1 1 255)", env, &err);
        chk("revoked: gfx write refused", err != NULL);

        // a revoked granted view as a gfx SOURCE draws nothing into a live dst.
        static uint8_t dst[64];
        memset(dst, 0x11, sizeof dst);
        lisp_value dstv = lisp_make_bytes_foreign(dst, sizeof dst, 0);
        lisp_env_define(env, lisp_make_symbol("dst", 3), dstv);
        err = NULL;
        lisp_eval_string("(gfx-blit! dst 4 1 1 0 0 gv 4 1 1)", env, &err);
        chk("revoked source: gfx-blit! is a no-op (dst untouched)", err == NULL && dst[0] == 0x11);

        // the generic readers must not leak the backing either: equal? compares,
        // hash-table hashes, and bytes-phys discloses an address. A dead view is
        // not equal? to anything (not even itself), hashes to a constant, and its
        // phys reads 0.
        // Compare against a SEPARATE live buffer with content identical to gv's
        // (revoked) backing: a non-hardened equal? would memcmp gv's backing and
        // return #t (the leak); hardened, a dead view is not equal? to anything.
        static uint8_t backing2[64];
        memcpy(backing2, backing, sizeof backing2);
        lisp_value gv2 = lisp_make_bytes_foreign(backing2, sizeof backing2, 0);
        lisp_env_define(env, lisp_make_symbol("gv2", 3), gv2);
        err = NULL;
        lisp_value eq = lisp_eval_string("(equal? gv gv2)", env, &err);
        chk("revoked: equal? on a dead view is #f (no memcmp oracle)", err == NULL && eq == LISP_FALSE);
        err = NULL;
        lisp_value ph = lisp_eval_string("(bytes-phys gv)", env, &err);
        chk("revoked: bytes-phys returns 0 (no address disclosure)", err == NULL && lisp_fixnum_val(ph) == 0);
        err = NULL;
        lisp_eval_string("(define h (make-hash-table)) (hash-set! h gv 1)", env, &err);
        chk("revoked: hash-set! on a dead key does not read the backing", err == NULL);
    }

    // 8b. `send` must not snapshot a revoked view's backing into another context
    //     (deep_copy zero-fills a dead grant view). A consumer receives the view and
    //     confirms it reads as all-zero, not the (reused) RAM the producer's view names.
    {
        lisp_sched_t s;
        lisp_sched_init(&s, 64);
        lisp_value env = lisp_default_env();
        lisp_install_sched(env);
        const char *err = NULL;
        char buf[64];

        static uint8_t backing[64];
        memset(backing, 0xAB, sizeof backing);     // "reused RAM" content
        lisp_value g = lisp_grant_mint(0x700000, sizeof backing, 1);
        uint32_t gi = 0, gg = 0;
        lisp_grant_handle(g, &gi, &gg);
        lisp_value view = lisp_make_bytes_foreign(backing, sizeof backing, 0x700000);
        lisp_bytes_set_grant(view, gi, gg);
        lisp_grant_revoke(g);                       // dead before the send

        lisp_env_define(env, lisp_make_symbol("dv", 2), view);
        lisp_eval_string(
            "(define consumer (spawn (lambda () (let ((b (recv))) (bytes-u8-ref b 0)))))"
            "(define producer (spawn (lambda () (send consumer dv))))",
            env, &err);
        if (err) printf("  (send setup error: %s)\n", err);
        lisp_value consumer = lisp_eval_string("consumer", env, &err);
        lisp_sched_run(&s, 0);
        chk("revoked view sent across (send) arrives zeroed, not 0xAB",
            strcmp(render(lisp_ctx_value(consumer), buf, sizeof buf), "0") == 0);
    }

    // 9. table exhaustion is a hard error, not a silent overwrite. Done last
    //    because it consumes the rest of the table for this process.
    int minted = 0;
    while (lisp_grant_mint(0x500000, 4096, 1) != LISP_UNDEF)
        minted++;
    chk("grant table eventually reports full (no silent overwrite)", minted > 0);

    printf("[lisp grant] %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
