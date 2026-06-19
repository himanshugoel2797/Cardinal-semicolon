// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// Host test for the driver substrate primitives (D1 bitwise + D2 mutable byte
// buffers). MMIO/DMA region minting is kernel-only (D3) and not covered here, but
// the byte-buffer accessors are exactly what an MMIO region uses, so exercising
// them on a heap buffer validates the access path.

#include <stdio.h>
#include <string.h>

#include "lisp.h"

static int failures = 0;
static int checks = 0;
static lisp_value g_env;

static void chk(const char *src, const char *want) {
    checks++;
    const char *err = NULL;
    lisp_value r = lisp_eval_string(src, g_env, &err);
    char buf[128];
    if (err != NULL)
        snprintf(buf, sizeof buf, "error: %s", err);
    else
        lisp_print(r, buf, sizeof buf);
    if (strcmp(buf, want) != 0) {
        printf("  FAIL %-52s got '%s' want '%s'\n", src, buf, want);
        failures++;
    } else {
        printf("  ok   %-52s -> %s\n", src, buf);
    }
}

int main(void) {
    uintptr_t stack_marker;
    lisp_gc_init(&stack_marker);
    g_env = lisp_default_env();
    lisp_install_sched(g_env);

    printf("[lisp driver] bitwise + bitfield + mutable byte buffers\n");

    // --- bitwise / shift ---
    chk("(bitwise-and 12 10)", "8");
    chk("(bitwise-or 12 10)", "14");
    chk("(bitwise-xor 12 10)", "6");
    chk("(bitwise-and 255 255 15)", "15");          // variadic
    chk("(bitwise-or)", "0");                         // identity
    chk("(bitwise-and)", "-1");                       // identity (all ones)
    chk("(bitwise-not 0)", "-1");
    chk("(arithmetic-shift 1 4)", "16");
    chk("(arithmetic-shift 256 -4)", "16");
    chk("(arithmetic-shift -8 -1)", "-4");            // arithmetic (sign-preserving)
    chk("(arithmetic-shift 1 60)", "1152921504606846976");

    // --- bitfields ---
    chk("(bit-extract 26 1 3)", "5");                 // bits 1..3 of 11010b = 101b
    chk("(bit-extract 4294967295 4 4)", "15");        // a u32 field
    chk("(bit-insert 0 4 3 5)", "80");                // put 5 into bits 4..6 => 5<<4
    chk("(bit-insert 255 0 4 0)", "240");             // clear the low nibble
    chk("(bit-insert (bit-insert 0 0 1 1) 4 3 5)", "81");  // enable + speed=5

    // --- mutable byte buffers (the MMIO/DMA access path) ---
    chk("(define r (make-bytes 16))", "r");
    chk("(bytes-length r)", "16");
    chk("(bytes-phys r)", "0");                       // heap buffer: no phys addr
    chk("(begin (bytes-u32-set! r 0 305419896) (bytes-u32-ref r 0))", "305419896");  // 0x12345678
    chk("(begin (bytes-u8-set! r 4 171) (bytes-u8-ref r 4))", "171");                // 0xAB
    // little-endian: a u32 written at 8 reads back byte-for-byte LE
    chk("(begin (bytes-u32-set! r 8 258) (bytes-u8-ref r 8))", "2");                 // 0x0102 -> low byte 2
    chk("(bytes-u8-ref r 9)", "1");
    chk("(begin (bytes-u16-set! r 12 4660) (bytes-u16-ref r 12))", "4660");          // 0x1234
    // bounds checks are enforced
    chk("(bytes-u32-ref r 13)", "error: bytes-ref: index out of range");
    chk("(bytes-u8-ref r 16)", "error: bytes-ref: index out of range");
    chk("(bytes-u8-ref r -1)", "error: bytes-ref: index out of range");
    chk("(bytes-u8-set! 5 0 0)", "error: bytes-set! expects (bytes index value)");

    // --- copy-on-send: a byte buffer survives crossing a context boundary ---
    {
        lisp_sched_t s;
        lisp_sched_init(&s, 256);
        s.per_context_heaps = 1;
        const char *err = NULL;
        lisp_eval_string(
            "(define sink"
            "  (spawn (lambda () (let ((m (recv))) (bytes-u32-ref m 0)))))"
            "(define src"
            "  (spawn (lambda () (let ((b (make-bytes 8)))"
            "                      (bytes-u32-set! b 0 3735928559) (send sink b) 'sent))))",
            g_env, &err);
        lisp_value sink = lisp_eval_string("sink", g_env, &err);
        lisp_sched_run(&s, 0);
        char buf[64];
        lisp_print(lisp_ctx_value(sink), buf, sizeof buf);
        checks++;
        if (err == NULL && strcmp(buf, "3735928559") == 0) {  // 0xDEADBEEF
            printf("  ok   %-52s -> %s\n", "bytes survive copy-on-send", buf);
        } else {
            printf("  FAIL %-52s got '%s'\n", "bytes survive copy-on-send", buf);
            failures++;
        }
    }

    printf("[lisp driver] %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
