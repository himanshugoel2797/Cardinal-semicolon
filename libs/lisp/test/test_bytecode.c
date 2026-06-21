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

// --- Randomized differential fuzzer -----------------------------------------
// Builds random expressions (as ASTs directly) over the supported subset and
// asserts the VM and the tree-walker agree -- both on values AND on which inputs
// error. Forms: arithmetic/comparison/if/and/or/not/let/immediately-applied
// lambda over fixnums, flonums, and booleans (the booleans deliberately flow
// into numeric positions to exercise the error-parity path + the prim fallback).

static uint64_t rng_state = 0x9e3779b97f4a7c15ULL;
static uint32_t rnd(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (uint32_t)(rng_state >> 32);
}

static lisp_value g_ops[16];  // interned operator/keyword symbols
enum { OPA_ADD, OPA_SUB, OPA_MUL, OPC_LT, OPC_LE, OPC_GT, OPC_GE, OPC_EQ,
       OP_S_IF, OP_S_AND, OP_S_OR, OP_S_NOT, OP_S_LET, OP_S_LAMBDA };
static lisp_value g_vpool[16];  // fresh variable-name symbols v0..v15

static void fuzz_init(void) {
    const char *names[] = {"+", "-", "*", "<", "<=", ">", ">=", "=",
                           "if", "and", "or", "not", "let", "lambda"};
    for (int i = 0; i < 14; i++)
        g_ops[i] = lisp_make_symbol(names[i], strlen(names[i]));
    for (int i = 0; i < 16; i++) {
        char nm[8];
        snprintf(nm, sizeof(nm), "v%d", i);
        g_vpool[i] = lisp_make_symbol(nm, strlen(nm));
    }
}

static lisp_value lst(int n, lisp_value *xs) {
    lisp_value r = LISP_EMPTY;
    for (int i = n - 1; i >= 0; i--)
        r = lisp_cons(xs[i], r);
    return r;
}

typedef struct {
    lisp_value vars[16];
    int nvars;
} fscope;

static lisp_value gen(fscope *s, int depth);

static lisp_value gen_atom(fscope *s) {
    int k = (int)(rnd() % 10);
    if (s->nvars > 0 && k < 4)
        return s->vars[rnd() % (uint32_t)s->nvars];
    if (k < 5)
        return (rnd() & 1) ? LISP_TRUE : LISP_FALSE;
    if (k < 6)
        return lisp_make_flonum((double)((int)(rnd() % 200) - 100) / 10.0);
    return lisp_fixnum((int)(rnd() % 41) - 20);
}

static lisp_value gen(fscope *s, int depth) {
    if (depth <= 0)
        return gen_atom(s);
    int k = (int)(rnd() % 9);
    lisp_value xs[4];
    switch (k) {
        case 0: {  // arithmetic
            xs[0] = g_ops[rnd() % 3];
            xs[1] = gen(s, depth - 1);
            xs[2] = gen(s, depth - 1);
            return lst(3, xs);
        }
        case 1: {  // comparison
            xs[0] = g_ops[OPC_LT + rnd() % 5];
            xs[1] = gen(s, depth - 1);
            xs[2] = gen(s, depth - 1);
            return lst(3, xs);
        }
        case 2: {  // if
            xs[0] = g_ops[OP_S_IF];
            xs[1] = gen(s, depth - 1);
            xs[2] = gen(s, depth - 1);
            xs[3] = gen(s, depth - 1);
            return lst(4, xs);
        }
        case 3: {  // and / or
            xs[0] = g_ops[(rnd() & 1) ? OP_S_AND : OP_S_OR];
            xs[1] = gen(s, depth - 1);
            xs[2] = gen(s, depth - 1);
            return lst(3, xs);
        }
        case 4: {  // not
            xs[0] = g_ops[OP_S_NOT];
            xs[1] = gen(s, depth - 1);
            return lst(2, xs);
        }
        case 5: {  // (let ((v INIT)) BODY)
            if (s->nvars >= 15)
                return gen_atom(s);
            lisp_value v = g_vpool[s->nvars];
            lisp_value init = gen(s, depth - 1);
            lisp_value bind = lisp_cons(v, lisp_cons(init, LISP_EMPTY));
            lisp_value binds = lisp_cons(bind, LISP_EMPTY);
            s->vars[s->nvars++] = v;
            lisp_value body = gen(s, depth - 1);
            s->nvars--;
            xs[0] = g_ops[OP_S_LET];
            xs[1] = binds;
            xs[2] = body;
            return lst(3, xs);
        }
        case 6: {  // ((lambda (v) BODY) ARG)
            if (s->nvars >= 15)
                return gen_atom(s);
            lisp_value v = g_vpool[s->nvars];
            lisp_value arg = gen(s, depth - 1);
            s->vars[s->nvars++] = v;
            lisp_value body = gen(s, depth - 1);
            s->nvars--;
            lisp_value params = lisp_cons(v, LISP_EMPTY);
            lisp_value lam = lisp_cons(g_ops[OP_S_LAMBDA],
                                       lisp_cons(params, lisp_cons(body, LISP_EMPTY)));
            xs[0] = lam;
            xs[1] = arg;
            return lst(2, xs);
        }
        default:
            return gen_atom(s);
    }
}

static int fuzz(lisp_value genv, int iters) {
    fuzz_init();
    int values = 0, both_err = 0, fails = 0, dec = 0;
    for (int i = 0; i < iters; i++) {
        fscope s;
        s.nvars = 0;
        lisp_value expr = gen(&s, 4);

        const char *eo = NULL;
        lisp_value ro = lisp_eval(expr, genv, &eo);
        lisp_value rv = LISP_UNDEF;
        const char *msg = NULL;
        lbc_status st = lbc_eval(genv, expr, &rv, &msg);

        if (st == LBC_DECLINED) {
            dec++;
            continue;
        }
        bool oe = (eo != NULL), ve = (st == LBC_ERR);
        if (oe || ve) {
            if (oe && ve) {
                both_err++;
            } else {
                fails++;
                if (fails <= 8) {
                    char b[256];
                    lisp_print(expr, b, sizeof(b));
                    printf("  FUZZ FAIL %s  (oracle %s, vm %s: %s)\n", b,
                           oe ? "err" : "ok", ve ? "err" : "ok",
                           oe ? (eo ? eo : "?") : (msg ? msg : "?"));
                }
            }
            continue;
        }
        if (vequal(ro, rv)) {
            values++;
        } else {
            fails++;
            if (fails <= 5) {
                char b[256], b1[64], b2[64];
                lisp_print(expr, b, sizeof(b));
                lisp_print(ro, b1, sizeof(b1));
                lisp_print(rv, b2, sizeof(b2));
                printf("  FUZZ FAIL %s\n            oracle=%s vm=%s\n", b, b1, b2);
            }
        }
    }
    printf("\n=== fuzz: %d random exprs ===\n", iters);
    printf("  %d value-match, %d error-parity, %d declined, %d FAILED\n", values,
           both_err, dec, fails);
    return fails;
}

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

    int fuzz_fails = fuzz(genv, 20000);
    bench(genv);

    int total_fail = fail + fuzz_fails;
    printf("\n[bytecode] %s\n", total_fail == 0 ? "ALL TESTS PASSED" : "FAILURES PRESENT");
    return total_fail == 0 ? 0 : 1;
}
