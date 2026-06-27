// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// Host test for the Intel e1000/e1000e NIC driver (lisp/drivers/e1000e.clp).
//
// The driver imports sys-mmio + sys-pci (kernel-only DMA/MMIO/MSI prims a host
// has no equivalent for). We register EMPTY stub modules under those names so the
// (import sys-mmio sys-pci ...) in the .clp resolves and the module loads with no
// hardware. We then assert ONLY the PURE, hardware-free exports over mock byte
// buffers -- the legacy descriptor build/parse and the MAC read:
//
//   tx-desc-build! / tx-desc-done?  -- TX descriptor field layout + DD readback
//   rx-desc-status / rx-desc-len / rx-desc-eop?  -- RX descriptor parse
//   read-mac                        -- RAL0/RAH0 -> 6 bytes
//
// The bring-up path (reset, ring/MSI setup, the RX pump / TX context) needs real
// hardware and is exercised in QEMU with `-device e1000e` / `-device e1000`, not
// here -- see the integration note. (Modeled on test_compositor.c's loader.)
//
// argv[1] is the test dir; from it we derive the lisp/ tree for the module loader.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lisp.h"

static char g_lispdir[1024];
static const char *const BASES[] = {"drivers", "lib", "servers"};

static bool e1000e_loader(const char *name, const char **src, size_t *len, void *ctx) {
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

// An empty stub builtin so sys-mmio / sys-pci resolve as modules. The driver's
// pure functions never call any of these caps, so a single inert export suffices.
static lisp_value stub(lisp_value *a, int n, const char **e) {
    (void)a;
    (void)n;
    (void)e;
    return LISP_FALSE;
}
static const lisp_builtin_export STUB[] = {{"__stub", stub}};

static const char *PROG =
    "(import e1000e driver-util)"
    "(define fails '())"
    "(define (ck name got want)"
    "  (if (equal? got want) #t (set! fails (cons (list name got want) fails))))"

    // --- read-mac: RAL0@0x5400 holds bytes 0..3 (LE), RAH0@0x5404 holds 4..5 ---
    // Build a regs buffer big enough to reach RAH0+4 (0x5408). Stamp a known MAC
    // 52:54:00:12:34:56 -> RAL0 = 0x00125452 (b0=0x52,b1=0x54,b2=0x00,b3=0x12),
    // RAH0 low 16 = 0x5634 (b4=0x34,b5=0x56).
    "(define regs (make-bytes #x5410))"
    "(bytes-u32-set! regs #x5400 #x12005452)"      // b3 b2 b1 b0  -> 0x12 0x00 0x54 0x52
    "(bytes-u32-set! regs #x5404 #x80005634)"      // AV(bit31) | b5 b4 -> 0x56 0x34
    "(ck 'mac (read-mac regs) (list #x52 #x54 #x00 #x12 #x34 #x56))"

    // --- TX descriptor build + done? over a 1-descriptor ring ---
    "(define tring (make-bytes 16))"
    // pre-stamp a buffer address so build! must NOT clobber it (it only touches
    // bytes 8..15). bufaddr lo@0 / hi@4.
    "(bytes-u32-set! tring 0 #xCAFE0000)"
    "(bytes-u32-set! tring 4 #x00000001)"
    "(ck 'tx-not-done-init (tx-desc-done? tring 0) #f)"   // DD clear initially
    "(tx-desc-build! tring 0 100)"
    "(ck 'tx-len   (bytes-u16-ref tring 8) 100)"          // length stamped
    "(ck 'tx-cmd   (bytes-u8-ref tring 11) #b1011)"       // EOP|IFCS|RS = 0x0B
    "(ck 'tx-addr-lo-kept (bytes-u32-ref tring 0) #xCAFE0000)"  // addr untouched
    "(ck 'tx-addr-hi-kept (bytes-u32-ref tring 4) #x00000001)"
    "(ck 'tx-status-clear (bytes-u8-ref tring 12) 0)"     // DD cleared by build!
    "(ck 'tx-not-done (tx-desc-done? tring 0) #f)"
    // NIC writes back DD into status byte (offset 12) -> done? flips true.
    "(bytes-u8-set! tring 12 1)"
    "(ck 'tx-done (tx-desc-done? tring 0) #t)"

    // --- RX descriptor parse: status / length / eop ---
    "(define rring (make-bytes 16))"
    "(ck 'rx-status-init (rx-desc-status rring 0) 0)"
    // NIC fills: length u16@8 = 60, status u8@12 = DD|EOP = 0x03.
    "(bytes-u16-set! rring 8 60)"
    "(bytes-u8-set! rring 12 #x03)"
    "(ck 'rx-status (rx-desc-status rring 0) #x03)"
    "(ck 'rx-len    (rx-desc-len rring 0) 60)"
    "(ck 'rx-eop    (rx-desc-eop? rring 0) #t)"
    // a descriptor with only DD (no EOP) -> eop? false (multi-buffer middle).
    "(bytes-u8-set! rring 12 #x01)"
    "(ck 'rx-eop-mid (rx-desc-eop? rring 0) #f)"

    "(if (null? fails) 'ALLOK (cons 'FAILED (reverse fails)))";

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    snprintf(g_lispdir, sizeof g_lispdir, "%s/../../../lisp", dir);

    uintptr_t stack_marker;
    lisp_gc_init(&stack_marker);
    lisp_value env = lisp_default_env();
    lisp_install_sched(env);
    lisp_set_module_loader(e1000e_loader, NULL);

    // Stub the kernel capability modules so the driver's (import sys-mmio sys-pci)
    // resolves with no hardware present.
    lisp_register_builtin_module(env, "sys-mmio", STUB, 1);
    lisp_register_builtin_module(env, "sys-pci", STUB, 1);

    printf("[lisp e1000e] pure TX/RX descriptor codecs + read-mac\n");

    const char *err = NULL;
    lisp_value r = lisp_eval_string(PROG, env, &err);
    if (err != NULL) {
        printf("  FAIL setup error: %s\n", err);
        return 1;
    }
    char buf[1024];
    lisp_print(r, buf, sizeof buf);
    if (strcmp(buf, "ALLOK") == 0) {
        printf("  ok   read-mac + tx-desc-build!/done? + rx-desc-status/len/eop?\n");
        return 0;
    }
    printf("  FAIL result: %s\n", buf);
    return 1;
}
