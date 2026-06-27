// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// Host test for the virtio-blk driver (lisp/drivers/virtio-blk.clp). The driver
// imports sys-mmio/sys-pci (the kernel DMA/MSI capabilities) plus driver-util +
// the shared virtio library -- none of which the host has. But the deliverable's
// PURE functions touch none of those: blk-build-header! formats the 16-byte
// request header into a caller-supplied bytes buffer, and blk-status-ok? maps a
// status byte to ok/error. So we register EMPTY stub sys-mmio/sys-pci modules
// (enough that (import virtio-blk) -- transitively (import virtio), which also
// pulls sys-mmio -- resolves) and assert the pure functions over mock buffers.
//
// What stays in-OS: the actual request virtqueue + corestorage registration,
// since those need real DMA/MSI + a live scheduler (the same split as the AHCI
// driver, whose request loop this mirrors).
//
// argv[1] is the test dir; from it we derive the lisp/ tree for the loader.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lisp.h"

static char g_lispdir[1024];
static const char *const BASES[] = {"servers", "lib", "drivers"};

static bool vblk_loader(const char *name, const char **src, size_t *len, void *ctx) {
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

// A single inert builtin so the stub modules are non-empty; never called by the
// pure functions under test.
static lisp_value stub(lisp_value *a, int n, const char **e) {
    (void)a;
    (void)n;
    (void)e;
    return LISP_FALSE;
}
static const lisp_builtin_export STUB[] = {{"__stub", stub}};

static const char *PROG =
    "(import virtio-blk driver-util)"
    "(define t (spawn (lambda ()"
    "  (define fails '())"
    "  (define (ck name got want)"
    "    (if (equal? got want) #t (set! fails (cons (list name got want) fails))))"
    // --- blk-build-header!: type @0, reserved @4 (zeroed), sector u64 @8 (LE) ---
    "  (let ((h (make-bytes 16)))"
    // pre-dirty the buffer so we prove reserved is actively zeroed.
    "    (bytes-u32-set! h 4 #xDEADBEEF)"
    "    (blk-build-header! h VIRTIO-BLK-T-IN #x1122)"
    "    (ck 'hdr-type    (bytes-u32-ref h 0) VIRTIO-BLK-T-IN)"
    "    (ck 'hdr-resv    (bytes-u32-ref h 4) 0)"
    "    (ck 'hdr-sec-lo  (bytes-u32-ref h 8) #x1122)"
    "    (ck 'hdr-sec-hi  (bytes-u32-ref h 12) 0)"
    // little-endian byte order of the sector low word: 0x1122 -> 22 11 00 00
    "    (ck 'hdr-byte0   (bytes-u8-ref h 8)  #x22)"
    "    (ck 'hdr-byte1   (bytes-u8-ref h 9)  #x11)"
    // a write header carries type=1; build over the same buffer to prove type is set.
    "    (blk-build-header! h VIRTIO-BLK-T-OUT 0)"
    "    (ck 'hdr-type-w  (bytes-u32-ref h 0) VIRTIO-BLK-T-OUT)"
    "    (ck 'hdr-sec0-lo (bytes-u32-ref h 8) 0)"
    // a high-LBA write: sector with a non-zero high word.
    "    (blk-build-header! h VIRTIO-BLK-T-OUT (arithmetic-shift 5 32))"
    "    (ck 'hdr-sec-hi5 (bytes-u32-ref h 12) 5)"
    "    (ck 'hdr-sec-lo0 (bytes-u32-ref h 8) 0))"
    // type constants are read vs write.
    "  (ck 'type-in  VIRTIO-BLK-T-IN  0)"
    "  (ck 'type-out VIRTIO-BLK-T-OUT 1)"
    // --- blk-status-ok?: 0 = OK; 1 (IOERR) / 2 (UNSUPP) / poison = error ---
    "  (ck 'status-ok    (blk-status-ok? 0) #t)"
    "  (ck 'status-ioerr (blk-status-ok? 1) #f)"
    "  (ck 'status-unsup (blk-status-ok? 2) #f)"
    "  (ck 'status-pois  (blk-status-ok? #xFF) #f)"
    "  (if (null? fails) 'ALLOK (cons 'FAILED (reverse fails))))))";

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    snprintf(g_lispdir, sizeof g_lispdir, "%s/../../../lisp", dir);

    uintptr_t stack_marker;
    lisp_gc_init(&stack_marker);
    lisp_value env = lisp_default_env();
    lisp_install_sched(env);
    lisp_set_module_loader(vblk_loader, NULL);

    // Stub the kernel capability modules the driver (and the virtio library it
    // imports) name, so (import virtio-blk) resolves on the host.
    lisp_register_builtin_module(env, "sys-mmio", STUB, 1);
    lisp_register_builtin_module(env, "sys-pci", STUB, 1);

    printf("[lisp virtio-blk] request header + status-byte pure helpers\n");

    lisp_sched_t s;
    lisp_sched_init(&s, 4000000);
    s.per_context_heaps = 1;
    const char *err = NULL;
    lisp_eval_string(PROG, env, &err);
    if (err != NULL) {
        printf("  FAIL setup error: %s\n", err);
        return 1;
    }
    lisp_value t = lisp_eval_string("t", env, &err);
    lisp_sched_run(&s, 0);

    char buf[1024];
    lisp_print(lisp_ctx_value(t), buf, sizeof buf);
    int done = (lisp_ctx_state(t) == LISP_CTX_DONE);
    if (done && err == NULL && strcmp(buf, "ALLOK") == 0) {
        printf("  ok   header formats type/reserved/sector (LE u64); status maps OK vs error\n");
        return 0;
    }
    printf("  FAIL state=%d err=%s\n  result: %s\n", lisp_ctx_state(t),
           err ? err : "(none)", buf);
    return 1;
}
