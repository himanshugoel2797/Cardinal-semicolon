// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// Host test for lisp/drivers/virtio-rng.clp. The device-backed half (queue
// posting, used-ring reaping, MMIO) needs kernel DMA/PCI authority a host
// cannot provide, so that path stays in-OS. What IS host-testable is the PURE
// slicing helper `rng-take`: given a buffered chunk (a (bytes fill consumed)
// pool) it copies the next requested run of bytes into a caller buffer and
// reports the new consumed offset + bytes copied. Exercising it on heap buffers
// validates exactly the partial-drain / refill-boundary logic the service uses.
//
// To even load the module we must satisfy its (import sys-mmio sys-pci ...):
// those are kernel built-in capability modules, absent on the host. We register
// EMPTY stub modules of those names so the import resolves -- the driver's
// device functions reference mmio-map/dma-alloc as FREE variables resolved only
// at call time, and the test never calls them, so a stub with no real exports
// is enough to load the module and reach rng-take.
//
// argv[1] is the test dir; from it we derive the lisp/ tree for the loader.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lisp.h"

static char g_lispdir[1024];
static const char *const BASES[] = {"drivers", "lib", "servers"};

static bool drv_loader(const char *name, const char **src, size_t *len, void *ctx) {
    (void)ctx;
    for (size_t b = 0; b < sizeof(BASES) / sizeof(BASES[0]); b++) {
        char path[1280];
        snprintf(path, sizeof(path), "%s/%s/%s.clp", g_lispdir, BASES[b], name);
        FILE *f = fopen(path, "rb");
        if (f == NULL)
            continue;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *buf = (char *)malloc((size_t)sz + 1);
        if (buf == NULL || fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
            fclose(f);
            return false;
        }
        buf[sz] = '\0';
        fclose(f);
        *src = buf;
        *len = (size_t)sz;
        return true;
    }
    return false;
}

// A single trivial export so the stub module is non-empty; never called.
static lisp_value stub(lisp_value *a, int n, const char **e) {
    (void)a;
    (void)n;
    (void)e;
    return LISP_FALSE;
}
static const lisp_builtin_export STUB[] = {{"__stub", stub}};

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    snprintf(g_lispdir, sizeof g_lispdir, "%s/../../../lisp", dir);

    uintptr_t stack_marker;
    lisp_gc_init(&stack_marker);
    lisp_value env = lisp_default_env();
    lisp_install_sched(env);
    lisp_set_module_loader(drv_loader, NULL);
    // Stub the kernel capability modules the driver imports.
    lisp_register_builtin_module(env, "sys-mmio", STUB, 1);
    lisp_register_builtin_module(env, "sys-pci", STUB, 1);

    printf("[lisp virtio-rng] pure rng-take slicing helper\n");

    const char *PROG =
        "(import virtio-rng)"
        "(define fails '())"
        "(define (ck name got want)"
        "  (if (equal? got want) #t (set! fails (cons (list name got want) fails))))"
        // A pool of 8 bytes [10 20 30 40 50 60 70 80], none consumed.
        "(define src (make-bytes 8))"
        "(bytes-u8-set! src 0 10) (bytes-u8-set! src 1 20) (bytes-u8-set! src 2 30)"
        "(bytes-u8-set! src 3 40) (bytes-u8-set! src 4 50) (bytes-u8-set! src 5 60)"
        "(bytes-u8-set! src 6 70) (bytes-u8-set! src 7 80)"
        // take 3 of 8 -> copies [10 20 30], consumed advances 0 -> 3.
        "(define out (make-bytes 8))"
        "(define r1 (rng-take (list src 8 0) out 0 3))"
        "(ck 'take3-consumed (car r1) 3)"
        "(ck 'take3-copied   (cadr r1) 3)"
        "(ck 'take3-b0 (bytes-u8-ref out 0) 10)"
        "(ck 'take3-b2 (bytes-u8-ref out 2) 30)"
        // continue from consumed=3, want 10 but only 5 remain -> copies [40..80].
        "(define r2 (rng-take (list src 8 3) out 3 10))"
        "(ck 'take-rest-consumed (car r2) 8)"
        "(ck 'take-rest-copied   (cadr r2) 5)"
        "(ck 'take-rest-b3 (bytes-u8-ref out 3) 40)"   // continues writing at out-off 3
        "(ck 'take-rest-b7 (bytes-u8-ref out 7) 80)"
        // pool exhausted (consumed == fill): copies nothing, consumed unchanged.
        "(define r3 (rng-take (list src 8 8) out 0 4))"
        "(ck 'take-empty-consumed (car r3) 8)"
        "(ck 'take-empty-copied   (cadr r3) 0)"
        // want 0 -> a no-op even with bytes available.
        "(define r4 (rng-take (list src 8 0) out 0 0))"
        "(ck 'take-zero-copied (cadr r4) 0)"
        "(ck 'take-zero-consumed (car r4) 0)"
        "(if (null? fails) 'ALLOK (cons 'FAILED (reverse fails)))";

    const char *err = NULL;
    lisp_value r = lisp_eval_string(PROG, env, &err);
    char buf[1024];
    lisp_print(r, buf, sizeof buf);
    if (err == NULL && strcmp(buf, "ALLOK") == 0) {
        printf("  ok   rng-take slices partial/exhausted/zero requests correctly\n");
        return 0;
    }
    printf("  FAIL err=%s\n  result: %s\n", err ? err : "(none)", buf);
    return 1;
}
