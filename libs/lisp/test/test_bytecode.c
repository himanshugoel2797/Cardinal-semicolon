// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// Differential test + microbenchmark for the prototype bytecode backend
// (lbc.inc), per notes/core/lisp-bytecode.md. For each program in a corpus it
// compiles+runs the expression through the bytecode VM and compares the result
// (and error-ness) bit-for-bit against the tree-walking evaluator (lisp_eval),
// the oracle. Anything the compiler declines is run on the oracle and reported,
// never failed -- decline-to-oracle keeps correctness a non-regression.
//
// Host-only: lbc.inc is #included here and nowhere else, so none of this enters
// the kernel build. The GC is left uninitialized (grow-only heap), so a bounded
// corpus has no rooting concerns.

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lisp.h"
#include "lbc.inc"

static void host_out(const char *s, size_t len, void *ctx) {
    (void)ctx;
    fwrite(s, 1, len, stdout);
}

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

// Structural equality, enough for the corpus result values.
static bool vequal(lisp_value a, lisp_value b) {
    if (a == b)
        return true;
    if (lisp_is_flonum(a) && lisp_is_flonum(b))
        return lisp_flonum_val(a) == lisp_flonum_val(b);
    if (lisp_is_pair(a) && lisp_is_pair(b))
        return vequal(lisp_car(a), lisp_car(b)) && vequal(lisp_cdr(a), lisp_cdr(b));
    if (lisp_is_string(a) && lisp_is_string(b)) {
        size_t la = lisp_string_len(a), lb = lisp_string_len(b);
        return la == lb && memcmp(lisp_string_data(a), lisp_string_data(b), la) == 0;
    }
    return false;
}

static lisp_value read1(const char *src) {
    const char *cur = src;
    const char *end = src + strlen(src);
    const char *err = NULL;
    lisp_value v = lisp_read(&cur, end, &err);
    if (err != NULL) {
        printf("  [reader error on %s: %s]\n", src, err);
        exit(2);
    }
    return v;
}

static int pass = 0, fail = 0, declined = 0;

static void check(lisp_value genv, const char *src) {
    lisp_value expr = read1(src);

    const char *eo = NULL;
    lisp_value ro = lisp_eval(expr, genv, &eo);

    lisp_value rv = LISP_UNDEF;
    const char *msg = NULL;
    lbc_status st = lbc_eval(genv, expr, &rv, &msg);

    char buf[256];
    if (st == LBC_DECLINED) {
        declined++;
        printf("  declined  %-52s (%s)\n", src, msg ? msg : "?");
        return;
    }
    bool oracle_err = (eo != NULL);
    bool vm_err = (st == LBC_ERR);
    if (oracle_err || vm_err) {
        if (oracle_err && vm_err) {
            pass++;
            printf("  ok(err)   %s\n", src);
        } else {
            fail++;
            printf("  FAIL      %s\n", src);
            printf("            oracle %s, vm %s\n",
                   oracle_err ? "errored" : "ok", vm_err ? "errored" : "ok");
        }
        return;
    }
    if (vequal(ro, rv)) {
        pass++;
        lisp_print(rv, buf, sizeof(buf));
        printf("  ok        %-52s => %s\n", src, buf);
    } else {
        fail++;
        char b2[256];
        lisp_print(ro, buf, sizeof(buf));
        lisp_print(rv, b2, sizeof(b2));
        printf("  FAIL      %s\n            oracle=%s  vm=%s\n", src, buf, b2);
    }
}

static const char *CORPUS[] = {
    // arithmetic + nesting + frozen ops
    "(+ 1 2)",
    "(* (+ 1 2) (- 10 4))",
    "(- 5)",                       // unary - : declines to general call path? no: arity1 -> generic
    "(+ 1 2 3 4)",                 // n-ary -> generic call (exact via prim)
    "(= (* 6 7) 42)",
    "(< 1 2)",
    "(>= 3 3)",
    // conditionals
    "(if (< 3 2) 100 200)",
    "(if (> 3 2) 'yes 'no)",
    "(when (< 1 2) 42)",
    "(unless (< 1 2) 42)",
    "(cond ((< 5 0) 'neg) ((= 5 0) 'zero) (else 'pos))",
    // and / or
    "(and 1 2 3)",
    "(and 1 #f 3)",
    "(or #f #f 7)",
    "(or #f 5 6)",
    "(and)",
    "(or)",
    // let family
    "(let ((x 5) (y 7)) (+ x y))",
    "(let* ((x 2) (y (* x x))) (+ x y))",
    "(let ((x 10)) ((lambda (y) (+ x y)) 5))",  // capture-by-value upvalue
    // lambda + application
    "((lambda (a b) (- a b)) 10 3)",
    "((lambda args args) 1 2 3)",               // rest arg
    // recursion
    "(let loop ((i 0) (acc 0)) (if (>= i 5) acc (loop (+ i 1) (+ acc i))))",
    "(let () (define (fact n) (if (<= n 1) 1 (* n (fact (- n 1))))) (fact 6))",
    "(let () (define (fib n) (if (< n 2) n (+ (fib (- n 1)) (fib (- n 2))))) (fib 10))",
    // list / pair frozen ops + prelude calls
    "(let ((p (cons 1 (cons 2 '())))) (car (cdr p)))",
    "(pair? (cons 1 2))",
    "(null? '())",
    "(null? (cons 1 2))",
    "(not (< 1 2))",
    "(quote (1 2 3))",
    "(length (list 1 2 3 4))",     // global call via lisp_apply
    "(let ((x 1)) (set! x 5) x)",  // set! on a non-captured local
    // flonum / mixed -> exact prim fallback
    "(+ 1.5 2.5)",
    "(< 1.5 2)",
    "(* 2.0 3)",
    // shared errors (both must error)
    "(car 5)",
    "(+ 1 'a)",
    // declines (run on oracle; reported, not failed)
    "(case 1 ((1) 'a) (else 'b))",
    "(letrec ((f (lambda (n) (if (< n 1) 0 (f (- n 1)))))) (f 3))",
};

static void bench(lisp_value genv) {
    // A tail-recursive counting loop -- the hot shape (named-let, frozen ops).
    const char *src =
        "(let loop ((i 0) (acc 0)) (if (= i 2000000) acc (loop (+ i 1) (+ acc 1))))";
    lisp_value expr = read1(src);

    bcchunk *k = NULL;
    const char *why = NULL;
    if (!lbc_compile(genv, expr, &k, &why)) {
        printf("\n[bench] compile declined: %s\n", why);
        return;
    }
    bcclosure *top = (bcclosure *)calloc(1, sizeof(bcclosure));
    top->chunk = k;

    // correctness once
    lisp_value rv = LISP_UNDEF, ro = LISP_UNDEF;
    const char *err = NULL, *eo = NULL;
    vm_run(top, genv, &rv, &err);
    ro = lisp_eval(expr, genv, &eo);
    char b1[64], b2[64];
    lisp_print(rv, b1, sizeof(b1));
    lisp_print(ro, b2, sizeof(b2));

    double t0 = now_sec();
    vm_run(top, genv, &rv, &err);
    double t_vm = now_sec() - t0;

    t0 = now_sec();
    (void)lisp_eval(expr, genv, &eo);
    double t_or = now_sec() - t0;

    printf("\n=== bench: 2,000,000-iteration tail loop ===\n");
    printf("  result      vm=%s oracle=%s %s\n", b1, b2,
           vequal(rv, ro) ? "(match)" : "(MISMATCH)");
    printf("  tree-walker %8.2f ms\n", t_or * 1000);
    printf("  bytecode VM %8.2f ms\n", t_vm * 1000);
    printf("  speedup     %8.2fx\n", t_or / t_vm);
}

int main(void) {
    lisp_set_output(host_out, NULL);
    lisp_value genv = lisp_default_env();

    printf("=== bytecode backend vs tree-walker (differential) ===\n");
    for (size_t i = 0; i < sizeof(CORPUS) / sizeof(CORPUS[0]); i++)
        check(genv, CORPUS[i]);

    printf("\n  %d passed, %d declined, %d FAILED\n", pass, declined, fail);

    bench(genv);

    printf("\n[bytecode] %s\n", fail == 0 ? "ALL TESTS PASSED" : "FAILURES PRESENT");
    return fail == 0 ? 0 : 1;
}
