// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// Host test for the coreusb descriptor parsers (lisp/servers/coreusb/proto.clp).
// The enumeration/transfer paths need a host controller, but the descriptor
// walkers and the string decoder are PURE byte-buffer code -- exactly the gap
// QEMU can't exercise with arbitrary descriptors -- so we load the real coreusb
// module through the same module-loader hook the kernel's initrd loader uses and
// assert the walkers against a synthetic composite (USB Audio) configuration.
//
// argv[1] is the test directory (libs/lisp/test), from which we derive the lisp/
// source tree so the loader can resolve coreusb + its includes + driver-util.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lisp.h"

// --- module loader: search the lisp/ source tree for <name>.clp --------------
// A qualified include name like "coreusb/proto" maps to <base>/coreusb/proto.clp;
// a bare module name like "driver-util" maps to <base>/driver-util.clp. We try
// each base dir in turn. Sources are read into malloc'd buffers and never freed
// (the process is short-lived); the reader is bounded, so no NUL terminator is
// required and we report the byte length.
static char g_lispdir[1024];
static const char *const BASES[] = {"servers", "lib", "drivers"};

static bool usb_loader(const char *name, const char **src, size_t *len, void *ctx) {
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

static int failures = 0;
static int checks = 0;
static lisp_value g_env;

static void chk(const char *src, const char *want) {
    checks++;
    const char *err = NULL;
    lisp_value r = lisp_eval_string(src, g_env, &err);
    char buf[256];
    if (err != NULL)
        snprintf(buf, sizeof buf, "error: %s", err);
    else
        lisp_print(r, buf, sizeof buf);
    if (strcmp(buf, want) != 0) {
        printf("  FAIL %-56s got '%s' want '%s'\n", src, buf, want);
        failures++;
    } else {
        printf("  ok   %-56s -> %s\n", src, buf);
    }
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    // <dir> is libs/lisp/test; the lisp/ tree is <dir>/../../../lisp.
    snprintf(g_lispdir, sizeof g_lispdir, "%s/../../../lisp", dir);

    uintptr_t stack_marker;
    lisp_gc_init(&stack_marker);
    g_env = lisp_default_env();
    lisp_install_sched(g_env);
    lisp_set_module_loader(usb_loader, NULL);

    printf("[lisp usb] coreusb descriptor parsers (multi-interface / alt / strings)\n");

    const char *err = NULL;
    lisp_eval_string("(import coreusb)", g_env, &err);
    if (err != NULL) {
        printf("  FAIL (import coreusb) -> error: %s\n", err);
        return 1;
    }

    // A list->bytevector helper + a length counter for building descriptors.
    lisp_eval_string(
        "(define (mkb lst)"
        "  (let loop ((b (make-bytes (cnt lst))) (i 0) (l lst))"
        "    (if (null? l) b (begin (bytes-u8-set! b i (car l)) (loop b (+ i 1) (cdr l))))))",
        g_env, &err);
    lisp_eval_string(
        "(define (cnt l) (if (null? l) 0 (+ 1 (cnt (cdr l)))))", g_env, &err);

    // A synthetic composite USB Audio configuration (69 bytes):
    //   config(9) IAD(8) AC-iface(9) AC-class-hdr(9) AS-iface-alt0(9)
    //   AS-iface-alt1(9) AS-class-gen(7) iso-OUT-endpoint(9)
    lisp_eval_string(
        "(define cfg (mkb (list"
        "  9 2 69 0 2 1 0 #x80 50"                 // configuration descriptor
        "  8 11 0 2 1 1 0 0"                        // IAD: function class 1 (audio)
        "  9 4 0 0 0 1 1 0 0"                       // iface 0 alt 0: AudioControl
        "  9 #x24 1 0 9 0 1 1 0"                    // AC class-specific header (skipped)
        "  9 4 1 0 0 1 2 0 0"                       // iface 1 alt 0: AS, 0 endpoints
        "  9 4 1 1 1 1 2 0 0"                       // iface 1 alt 1: AS, 1 endpoint
        "  7 #x24 1 1 0 2 0"                        // AS class-specific general (skipped)
        "  9 5 #x01 #x09 192 0 1 0 0)))"            // EP 0x01 OUT, iso/adaptive, mps 192
        , g_env, &err);
    lisp_eval_string("(define dev (list 0 1 1 8 cfg 69))", g_env, &err);
    if (err != NULL) {
        printf("  FAIL building descriptors -> error: %s\n", err);
        return 1;
    }

    // --- interface walk: IAD skipped, 3 interface (alt) descriptors found ---
    chk("(cnt (usb-interfaces dev))", "3");
    chk("(iface-class (car (usb-interfaces dev)))", "1");       // AC
    chk("(iface-subclass (car (usb-interfaces dev)))", "1");
    chk("(iface-number (caddr (usb-interfaces dev)))", "1");
    chk("(iface-alt (caddr (usb-interfaces dev)))", "1");
    chk("(iface-subclass (caddr (usb-interfaces dev)))", "2");  // AudioStreaming
    chk("(iface-num-eps (caddr (usb-interfaces dev)))", "1");

    // --- first-interface accessors (back-compat path) ---
    chk("(usb-iface-class dev)", "1");
    chk("(usb-iface-subclass dev)", "1");
    chk("(usb-iface-number dev)", "0");

    // --- endpoint scoping by (interface, alt) ---
    chk("(cnt (usb-iface-endpoints dev 1 0))", "0");            // zero-bandwidth alt
    chk("(cnt (usb-iface-endpoints dev 1 1))", "1");            // active alt
    chk("(ep-type (car (usb-iface-endpoints dev 1 1)))", "1");  // isochronous
    chk("(ep-sync-type (car (usb-iface-endpoints dev 1 1)))", "2");
    chk("(ep-dir-in? (car (usb-iface-endpoints dev 1 1)))", "#f");
    chk("(ep-number (car (usb-iface-endpoints dev 1 1)))", "1");
    chk("(ep-max-packet (car (usb-iface-endpoints dev 1 1)))", "192");
    chk("(ep-interval (car (usb-iface-endpoints dev 1 1)))", "1");
    chk("(ep-address (usb-find-ep-in (usb-iface-endpoints dev 1 1) 1 #f))", "1");
    chk("(usb-find-ep-in (usb-iface-endpoints dev 1 1) 1 #t)", "#f"); // no iso IN
    chk("(usb-iface-endpoints dev 9 0)", "()");                 // no such interface

    // --- UTF-16LE string descriptor decode (pure) ---
    chk("(usb-string-decode (mkb (list 6 3 72 0 105 0)))", "\"Hi\"");
    chk("(usb-string-decode (mkb (list 8 3 72 0 233 0 105 0)))", "\"H?i\"");  // non-ASCII -> ?
    chk("(usb-string-decode (mkb (list 2 3)))", "\"\"");        // empty body

    // --- with-retries: success path (no sleep -- the retry path needs a context) ---
    chk("(with-retries 3 0 (lambda (r) (= r 7)) (lambda () 7))", "7");     // ok? true first try
    chk("(with-retries 1 0 (lambda (r) #f) (lambda () 9))", "9");          // tries exhausted -> last
    chk("(with-retries 3 0 (lambda (r) (>= r 0)) (lambda () 5))", "5");    // ok? short-circuits

    printf("\n[lisp usb] %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
