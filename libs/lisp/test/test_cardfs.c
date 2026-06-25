// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// Host test for the crash-consistent, integrity-checked cardfs object store
// (lisp/servers/cardfs.clp). The on-disk persistence + crash-recovery path is
// exactly what QEMU can't exercise (you can't reliably kill it mid-write), so we
// stand a fault-injecting in-memory RAM disk in for a block driver: besides the
// CoreStorage (read lba cnt reply)/(write lba cnt data reply) protocol it answers
// control messages (peek/poke/drop-lba) that let the test corrupt a specific block
// or DROP a specific write -- modelling a torn write / lost commit / bit-rot
// deterministically. The real corestorage service mediates I/O and the real cardfs
// provider does format/put/get/delete/stat/keys/get-range over the message API.
//
// We then re-mount with a FRESH provider (modelling a reboot) and assert recovery:
//   - clean round-trip incl. multi-block objects, replace (last-writer-wins),
//     stat, key enumeration, ranged read, delete;
//   - a lost commit superblock write rolls the put back entirely (atomicity);
//   - bit-rot in a data block is reported 'corrupt, not served;
//   - a destroyed active superblock falls back to the older valid checkpoint;
//   - both superblocks corrupt -> the volume declines to mount (no crash);
//   - a full device returns 'full and stays consistent across remount.
//
// argv[1] is the test dir (libs/lisp/test); from it we derive the lisp/ tree so
// the module loader can resolve cardfs + corestorage + driver-util.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lisp.h"

static char g_lispdir[1024];
static const char *const BASES[] = {"servers", "lib", "drivers"};

static bool cardfs_loader(const char *name, const char **src, size_t *len, void *ctx) {
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

// The whole scenario, driven by one `t` context under the real scheduler. It
// accumulates failures into `fails` and returns 'ALLOK iff every check passed.
static const char *PROG =
    "(import corestorage cardfs driver-util)"
    // string <-> bytes
    "(define (s->b s) (let ((b (make-bytes (string-length s))))"
    "  (let loop ((i 0)) (if (< i (string-length s))"
    "    (begin (bytes-u8-set! b i (char->integer (string-ref s i))) (loop (+ i 1))) b))))"
    "(define (b->s b) (let loop ((i 0) (acc (quote ())))"
    "  (if (< i (bytes-length b)) (loop (+ i 1) (cons (integer->char (bytes-u8-ref b i)) acc))"
    "    (list->string (reverse acc)))))"
    "(define (rep ch n) (let loop ((i 0) (acc (quote ())))"
    "  (if (>= i n) (list->string acc) (loop (+ i 1) (cons ch acc)))))"
    "(define (mem? x lst) (cond ((null? lst) #f) ((= x (car lst)) #t) (else (mem? x (cdr lst)))))"
    "(define (rem1 x lst) (cond ((null? lst) (quote ())) ((= x (car lst)) (cdr lst))"
    "  (else (cons (car lst) (rem1 x (cdr lst))))))"
    "(define (garbage) (let ((b (make-bytes 512)))"
    "  (let loop ((i 0)) (if (>= i 512) b (begin (bytes-u8-set! b i 219) (loop (+ i 1)))))))"
    // fault-injecting RAM disk: nblocks x 512B, plus peek/poke/drop-lba controls
    "(define (make-disk nblocks) (spawn (lambda ()"
    "  (let ((ram (make-bytes (* nblocks 512))) (drop (quote ())))"
    "    (let loop ()"
    "      (let ((m (recv)))"
    "        (cond"
    "          ((eq? (car m) (quote read))"
    "           (send (cadddr m) (list (quote complete) 0 (copy-bytes ram (* (cadr m) 512) (* (caddr m) 512)))))"
    "          ((eq? (car m) (quote write))"
    "           (let ((lba (cadr m)))"
    "             (if (mem? lba drop) (set! drop (rem1 lba drop))"
    "                 (bytes-copy-into! ram (* lba 512) (cadddr m) (* (caddr m) 512)))"
    "             (send (nth m 4) (list (quote complete) 0))))"
    "          ((eq? (car m) (quote peek))"
    "           (send (caddr m) (list (quote complete) 0 (copy-bytes ram (* (cadr m) 512) 512))))"
    "          ((eq? (car m) (quote poke))"
    "           (bytes-copy-into! ram (* (cadr m) 512) (caddr m) 512)"
    "           (send (cadddr m) (list (quote complete) 0)))"
    "          ((eq? (car m) (quote drop-lba))"
    "           (set! drop (cons (cadr m) drop)) (send (caddr m) (list (quote complete) 0))))"
    "        (loop)))))))"
    "(define stor (start-storage-service))"
    "(define cur (start-cardfs stor))"
    "(define fails (quote ()))"
    "(define (ck tag got want) (if (not (equal? got want)) (set! fails (cons (list tag got want) fails))))"
    "(define (reg name disk bc) (send stor (list (quote register-blockdev) name 512 bc disk)))"
    "(define (fmt name bc) (send cur (list (quote format) stor name bc (self))) (cadr (recv)))"
    "(define (put name k v) (send cur (list (quote put) stor name k (s->b v) (self))) (cadr (recv)))"
    "(define (del name k) (send cur (list (quote delete) stor name k (self))) (cadr (recv)))"
    "(define (gv name k) (send cur (list (quote get) stor name k (self)))"
    "  (let ((r (recv))) (if (eq? (cadr r) (quote ok)) (b->s (caddr r)) (cadr r))))"
    "(define (grv name k off len) (send cur (list (quote get-range) stor name k off len (self)))"
    "  (let ((r (recv))) (if (eq? (cadr r) (quote ok)) (b->s (caddr r)) (cadr r))))"
    "(define (stt name k) (send cur (list (quote stat) stor name k (self))) (cadr (recv)))"
    "(define (kc name) (send cur (list (quote keys) stor name (self)))"
    "  (let ((r (cadr (recv)))) (if (list? r) (length r) r)))"
    "(define (poke disk lba b) (send disk (list (quote poke) lba b (self))) (recv))"
    "(define (drop disk lba) (send disk (list (quote drop-lba) lba (self))) (recv))"
    "(define (remount name disk bc) (let ((p (start-cardfs stor)))"
    "  (send p (list (quote probe) name 512 bc disk stor)) (set! cur p)))"
    "(define BIG (rep (integer->char 66) 300))"  // 300 'B's -> multi-block object
    "(define t (spawn (lambda ()"
    // --- Scenario 1+2: functional round-trip, then reboot persistence ---
    "  (let ((d (make-disk 64)))"
    "    (reg (quote v1) d 64)"
    "    (ck (quote fmt1) (fmt (quote v1) 64) (quote ok))"
    "    (ck (quote put-a) (put (quote v1) \"alpha\" \"one\") (quote ok))"
    "    (ck (quote put-b) (put (quote v1) \"beta\" BIG) (quote ok))"
    "    (ck (quote get-a) (gv (quote v1) \"alpha\") \"one\")"
    "    (ck (quote get-b) (gv (quote v1) \"beta\") BIG)"
    "    (put (quote v1) \"alpha\" \"ONE-TWO\")"  // replace
    "    (ck (quote replace) (gv (quote v1) \"alpha\") \"ONE-TWO\")"
    "    (ck (quote count2) (kc (quote v1)) 2)"
    "    (ck (quote stat-a) (stt (quote v1) \"alpha\") (list 7 3))"  // size 7, id 3 (a=1,b=2,a'=3)
    "    (ck (quote range-b) (grv (quote v1) \"beta\" 10 5) (rep (integer->char 66) 5))"
    "    (ck (quote range-oob) (grv (quote v1) \"beta\" 298 5) (quote range))"
    "    (ck (quote del-a) (del (quote v1) \"alpha\") (quote ok))"
    "    (ck (quote get-a-miss) (gv (quote v1) \"alpha\") (quote miss))"
    "    (ck (quote count1) (kc (quote v1)) 1)"
    "    (remount (quote v1) d 64)"  // reboot
    "    (ck (quote reboot-b) (gv (quote v1) \"beta\") BIG)"
    "    (ck (quote reboot-a-gone) (gv (quote v1) \"alpha\") (quote miss))"
    "    (ck (quote reboot-count) (kc (quote v1)) 1))"
    // --- Scenario 3: lost commit superblock write -> put rolls back ---
    "  (let ((d (make-disk 64)))"
    "    (reg (quote v3) d 64)"
    "    (ck (quote fmt3) (fmt (quote v3) 64) (quote ok))"
    "    (ck (quote p3-k1) (put (quote v3) \"k1\" \"v1\") (quote ok))"
    "    (drop d 0)"  // k2's commit SB targets slot 0 -> drop it (lost write)
    "    (put (quote v3) \"k2\" \"v2\")"
    "    (remount (quote v3) d 64)"
    "    (ck (quote torn-k1) (gv (quote v3) \"k1\") \"v1\")"
    "    (ck (quote torn-k2) (gv (quote v3) \"k2\") (quote miss)))"
    // --- Scenario 4: bit-rot in a data block -> 'corrupt, neighbour ok ---
    "  (let ((d (make-disk 64)))"
    "    (reg (quote v4) d 64)"
    "    (fmt (quote v4) 64)"
    "    (put (quote v4) \"x\" \"hello\")"  // hdr@2 data@3
    "    (put (quote v4) \"y\" \"world\")"  // hdr@4 data@5
    "    (poke d 3 (garbage))"
    "    (remount (quote v4) d 64)"
    "    (ck (quote rot-x) (gv (quote v4) \"x\") (quote corrupt))"
    "    (ck (quote rot-y) (gv (quote v4) \"y\") \"world\"))"
    // --- Scenario 5: destroyed active superblock -> fall back to older ---
    "  (let ((d (make-disk 64)))"
    "    (reg (quote v5) d 64)"
    "    (fmt (quote v5) 64)"  // slot0 gen1 active0
    "    (put (quote v5) \"p\" \"1\")"  // slot1 gen2 active1
    "    (put (quote v5) \"q\" \"2\")"  // slot0 gen3 active0
    "    (poke d 0 (garbage))"  // destroy the live checkpoint
    "    (remount (quote v5) d 64)"  // only slot1 (gen2) survives
    "    (ck (quote s5-p) (gv (quote v5) \"p\") \"1\")"
    "    (ck (quote s5-q) (gv (quote v5) \"q\") (quote miss)))"
    // --- Scenario 6: both superblocks corrupt -> no mount, no crash ---
    "  (let ((d (make-disk 64)))"
    "    (reg (quote v6) d 64)"
    "    (fmt (quote v6) 64) (put (quote v6) \"z\" \"9\")"
    "    (poke d 0 (garbage)) (poke d 1 (garbage))"
    "    (remount (quote v6) d 64)"
    "    (ck (quote s6) (gv (quote v6) \"z\") (quote no-volume)))"
    // --- Scenario 8: full device -> 'full, consistent across remount ---
    "  (let ((d (make-disk 5)))"
    "    (reg (quote v8) d 5)"
    "    (fmt (quote v8) 5)"
    "    (ck (quote s8-a) (put (quote v8) \"a\" \"x\") (quote ok))"  // hdr2 data3 -> head4
    "    (ck (quote s8-full) (put (quote v8) \"b\" \"y\") (quote full))"  // head4+2 > 5
    "    (remount (quote v8) d 5)"
    "    (ck (quote s8-a2) (gv (quote v8) \"a\") \"x\")"
    "    (ck (quote s8-b2) (gv (quote v8) \"b\") (quote miss)))"
    // --- Scenario 9: delete on a full log must report 'full, not fake success ---
    // Fill the 3 log blocks with header-only (empty) puts so head == bcount; a
    // tombstone needs one more block -> 'full. delete must NOT report 'ok (which
    // would drop the key in RAM while no tombstone is committed -> resurrects on
    // remount). The discriminating check is delete's return value.
    "  (let ((d (make-disk 5)))"
    "    (reg (quote v9) d 5)"
    "    (fmt (quote v9) 5)"
    "    (ck (quote s9-a) (put (quote v9) \"a\" \"\") (quote ok))"  // hdr@2 head->3
    "    (ck (quote s9-b) (put (quote v9) \"b\" \"\") (quote ok))"  // hdr@3 head->4
    "    (ck (quote s9-c) (put (quote v9) \"c\" \"\") (quote ok))"  // hdr@4 head->5 (==bcount)
    "    (ck (quote s9-del-full) (del (quote v9) \"a\") (quote full))"
    "    (remount (quote v9) d 5)"
    "    (ck (quote s9-survive) (gv (quote v9) \"a\") \"\"))"  // never durably deleted
    "  (if (null? fails) (quote ALLOK) (cons (quote FAILED) (reverse fails))))))";

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    snprintf(g_lispdir, sizeof g_lispdir, "%s/../../../lisp", dir);

    uintptr_t stack_marker;
    lisp_gc_init(&stack_marker);
    lisp_value env = lisp_default_env();
    lisp_install_sched(env);
    lisp_set_module_loader(cardfs_loader, NULL);

    printf("[lisp cardfs] crash-consistency + integrity (log-structured store)\n");

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
        printf("  ok   all scenarios passed (format/put/get/delete/stat/keys/range\n");
        printf("       + lost-commit rollback + bit-rot detect + SB fallback + full)\n");
        return 0;
    }
    printf("  FAIL state=%d err=%s\n  result: %s\n", lisp_ctx_state(t),
           err ? err : "(none)", buf);
    return 1;
}
