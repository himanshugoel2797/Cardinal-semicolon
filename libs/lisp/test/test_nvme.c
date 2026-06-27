// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// Host test for the NVMe driver (lisp/drivers/nvme.clp). NVMe bring-up needs real
// MMIO/DMA/PCI hardware, so what is host-testable is the PURE layer: the SQE
// builder, the CQE parser, and the IDENTIFY-NAMESPACE parser. We register EMPTY
// stub sys-mmio / sys-pci modules so (import nvme) resolves its capability imports
// (the pure functions never call a sys-* prim, so the stubs are never invoked),
// then assert the pure functions over mock byte buffers we lay out by hand.
//
// argv[1] is the test dir (libs/lisp/test); from it we derive the lisp/ tree for
// the module loader (model on test_compositor.c's comp_loader).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lisp.h"

static char g_lispdir[1024];
static const char *const BASES[] = {"servers", "lib", "drivers"};

static bool nvme_loader(const char *name, const char **src, size_t *len, void *ctx) {
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

// A do-nothing primitive: the stub sys-* modules expose one of these so the
// import resolves. The pure functions never call it.
static lisp_value stub(lisp_value *a, int n, const char **e) {
    (void)a;
    (void)n;
    (void)e;
    return LISP_FALSE;
}
static const lisp_builtin_export STUB[] = {{"__stub", stub}};

static const char *PROG =
    "(import nvme driver-util)"
    "(define t (spawn (lambda ()"
    "  (define fails '())"
    "  (define (ck name got want)"
    "    (if (equal? got want) #t (set! fails (cons (list name got want) fails))))"

    // --- 1. nvme-cmd-build! lays out a 64-byte SQE correctly --------------
    "  (let ((sqe (make-bytes 64)))"
    // pre-dirty so we prove the builder zeroes the whole 64 bytes
    "    (let loop ((i 0)) (if (< i 64) (begin (bytes-u8-set! sqe i #xEE) (loop (+ i 1))) 'z))"
    // OPC=0x02 (READ), CID=0x1234, NSID=1, PRP1=0x0000ABCD00001000,
    // PRP2=0x0000ABCD00002000, cdw10=0x11111111 cdw11=0x22222222 cdw12=0x7
    "    (nvme-cmd-build! sqe NVME-OP-READ #x1234 1"
    "                     #xABCD00001000 #xABCD00002000"
    "                     (list #x11111111 #x22222222 #x7))"
    // CDW0: OPC in 7:0, CID in 31:16
    "    (ck 'sqe-cdw0 (bytes-u32-ref sqe 0) (bitwise-or #x02 (arithmetic-shift #x1234 16)))"
    "    (ck 'sqe-nsid (bytes-u32-ref sqe 4) 1)"
    // reserved dwords 2,3 zeroed
    "    (ck 'sqe-rsv2 (bytes-u32-ref sqe 8) 0)"
    "    (ck 'sqe-rsv3 (bytes-u32-ref sqe 12) 0)"
    // MPTR (dword4,5) zeroed
    "    (ck 'sqe-mptr (bytes-u32-ref sqe 16) 0)"
    // PRP1 (dword6 lo, dword7 hi) at bytes 24/28
    "    (ck 'sqe-prp1lo (bytes-u32-ref sqe 24) #x00001000)"
    "    (ck 'sqe-prp1hi (bytes-u32-ref sqe 28) #x0000ABCD)"
    // PRP2 (dword8 lo, dword9 hi) at bytes 32/36
    "    (ck 'sqe-prp2lo (bytes-u32-ref sqe 32) #x00002000)"
    "    (ck 'sqe-prp2hi (bytes-u32-ref sqe 36) #x0000ABCD)"
    // CDW10..12 at bytes 40/44/48
    "    (ck 'sqe-cdw10 (bytes-u32-ref sqe 40) #x11111111)"
    "    (ck 'sqe-cdw11 (bytes-u32-ref sqe 44) #x22222222)"
    "    (ck 'sqe-cdw12 (bytes-u32-ref sqe 48) #x7)"
    // CDW13..15 (no values supplied) zeroed
    "    (ck 'sqe-cdw13 (bytes-u32-ref sqe 52) 0)"
    "    (ck 'sqe-cdw15 (bytes-u32-ref sqe 60) 0))"

    // --- 2. CQE parse: status + phase + cid ------------------------------
    // Build a CQE dword3: CID=0x1234 (15:0), Phase=1 (bit16), Status=0 (31:17).
    "  (let ((cqe (make-bytes 16)))"
    "    (bytes-u32-set! cqe 12 (bitwise-or #x1234 (arithmetic-shift 1 16)))"
    "    (ck 'cqe-phase-1 (cq-phase cqe 0) 1)"
    "    (ck 'cqe-status-ok (cq-status cqe 0) 0)"
    // phase 0 + a non-zero status code (SC=0x05 in bits 24:17 -> dword3 bit 17+...)
    "    (bytes-u32-set! cqe 12 (arithmetic-shift #x05 17))"   // status field = 5, phase 0
    "    (ck 'cqe-phase-0 (cq-phase cqe 0) 0)"
    "    (ck 'cqe-status-err (cq-status cqe 0) #x05))"
    // a CQE at a non-zero offset (slot 1 in a 16-byte-entry CQ -> offset 16)
    "  (let ((cq (make-bytes 64)))"
    "    (bytes-u32-set! cq (+ 16 12) (bitwise-or #x0007 (arithmetic-shift 1 16)))"
    "    (ck 'cqe-off-phase (cq-phase cq 16) 1)"
    "    (ck 'cqe-off-status (cq-status cq 16) 0))"

    // --- 3. IDENTIFY NAMESPACE parse -------------------------------------
    // NSZE @0 (u64), FLBAS @26, LBAF list @128 (4 bytes each, LBADS bits 23:16).
    "  (let ((idns (make-bytes 4096)))"
    // NSZE = 0x00000000_00100000 (1,048,576 blocks)
    "    (bytes-u32-set! idns 0 #x00100000)"
    "    (bytes-u32-set! idns 4 0)"
    // FLBAS selects format 0; LBAF0 has LBADS=9 (512B) in bits 23:16
    "    (bytes-u8-set! idns 26 0)"
    "    (bytes-u32-set! idns 128 (arithmetic-shift 9 16))"
    "    (ck 'idns-nsze (id-ns-nsze idns) #x100000)"
    "    (ck 'idns-lbads (id-ns-lbads idns) 9)"
    "    (ck 'idns-bsize (id-ns-bsize idns) 512)"
    // FLBAS selects format 1; LBAF1 has LBADS=12 (4096B)
    "    (bytes-u8-set! idns 26 1)"
    "    (bytes-u32-set! idns (+ 128 4) (arithmetic-shift 12 16))"
    "    (ck 'idns-lbads-4k (id-ns-lbads idns) 12)"
    "    (ck 'idns-bsize-4k (id-ns-bsize idns) 4096)"
    // garbage LBADS (e.g. 0) falls back to 512
    "    (bytes-u8-set! idns 26 2)"
    "    (bytes-u32-set! idns (+ 128 8) 0)"
    "    (ck 'idns-bsize-guard (id-ns-bsize idns) 512))"

    "  (if (null? fails) 'ALLOK (cons 'FAILED (reverse fails))))))";

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    snprintf(g_lispdir, sizeof g_lispdir, "%s/../../../lisp", dir);

    uintptr_t stack_marker;
    lisp_gc_init(&stack_marker);
    lisp_value env = lisp_default_env();
    lisp_install_sched(env);
    lisp_register_builtin_module(env, "sys-mmio", STUB, 1);
    lisp_register_builtin_module(env, "sys-pci", STUB, 1);
    lisp_set_module_loader(nvme_loader, NULL);

    printf("[lisp nvme] pure SQE builder + CQE parse + IDENTIFY-NS parse\n");

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

    char buf[2048];
    lisp_print(lisp_ctx_value(t), buf, sizeof buf);
    int done = (lisp_ctx_state(t) == LISP_CTX_DONE);
    if (done && err == NULL && strcmp(buf, "ALLOK") == 0) {
        printf("  ok   SQE field layout, CQE phase/status, IDENTIFY-NS NSZE/LBADS\n");
        return 0;
    }
    printf("  FAIL state=%d err=%s\n  result: %s\n", lisp_ctx_state(t),
           err ? err : "(none)", buf);
    return 1;
}
