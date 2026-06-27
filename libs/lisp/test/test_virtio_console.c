// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// Host test for lisp/drivers/virtio-console.clp. The device-backed half (the
// transmitq/receiveq, MMIO, the blocking used-ring wait) needs kernel DMA/PCI
// authority a host cannot provide, so it stays in-OS. What IS host-testable is
// the PURE packer `console-pack`: it normalises a string (or an already-packed
// bytes) into a fresh contiguous byte buffer -- exactly the payload the TX path
// hands to the device. We assert it copies char codes faithfully and passes a
// bytes argument through unchanged.
//
// As in the rng test, loading the module needs its (import sys-mmio sys-pci ...)
// to resolve; those kernel capability modules are absent on the host, so we
// register EMPTY stubs of those names. The driver's device functions name
// mmio-map/dma-alloc as free variables resolved only when CALLED, and the test
// calls only console-pack, so empty stubs suffice to load the module.
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
    lisp_register_builtin_module(env, "sys-mmio", STUB, 1);
    lisp_register_builtin_module(env, "sys-pci", STUB, 1);

    printf("[lisp virtio-console] pure console-pack string->bytes packer\n");

    const char *PROG =
        "(import virtio-console)"
        "(define fails '())"
        "(define (ck name got want)"
        "  (if (equal? got want) #t (set! fails (cons (list name got want) fails))))"
        // pack "Hi" -> a 2-byte buffer with codes 72 ('H') 105 ('i').
        "(define p (console-pack \"Hi\"))"
        "(ck 'pack-len   (bytes-length p) 2)"
        "(ck 'pack-H     (bytes-u8-ref p 0) 72)"
        "(ck 'pack-i     (bytes-u8-ref p 1) 105)"
        // a newline-terminated banner-style string packs its trailing 10.
        "(define q (console-pack \"A\\n\"))"
        "(ck 'pack-A     (bytes-u8-ref q 0) 65)"
        "(ck 'pack-nl    (bytes-u8-ref q 1) 10)"
        // an empty string yields a 1-byte (placeholder) buffer, never zero-length.
        "(ck 'pack-empty-len (bytes-length (console-pack \"\")) 1)"
        // a bytes argument passes through unchanged (same object, same contents).
        "(define b (make-bytes 3))"
        "(bytes-u8-set! b 0 1) (bytes-u8-set! b 1 2) (bytes-u8-set! b 2 3)"
        "(define pb (console-pack b))"
        "(ck 'pack-bytes-same (eq? pb b) #t)"
        "(ck 'pack-bytes-b1   (bytes-u8-ref pb 1) 2)"
        "(if (null? fails) 'ALLOK (cons 'FAILED (reverse fails)))";

    const char *err = NULL;
    lisp_value r = lisp_eval_string(PROG, env, &err);
    char buf[1024];
    lisp_print(r, buf, sizeof buf);
    if (err == NULL && strcmp(buf, "ALLOK") == 0) {
        printf("  ok   console-pack packs strings + passes bytes through\n");
        return 0;
    }
    printf("  FAIL err=%s\n  result: %s\n", err ? err : "(none)", buf);
    return 1;
}
