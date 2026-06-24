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

// Compare an engine's outcome against the oracle. 0 = agree (value match or both
// error), 1 = disagree, 2 = engine declined.
static int diff_engine(lisp_value ro, bool oe, lbc_status st, lisp_value rv) {
    if (st == LBC_DECLINED)
        return 2;
    bool ve = (st == LBC_ERR);
    if (oe || ve)
        return (oe && ve) ? 0 : 1;
    return vequal(ro, rv) ? 0 : 1;
}

// Run one engine and fold its result into the counters / output.
static void run_engine(const char *eng, const char *src, lisp_value ro, bool oe,
                       lbc_status st, lisp_value rv, const char *msg) {
    int d = diff_engine(ro, oe, st, rv);
    if (d == 2) {
        declined++;
        printf("  declined[%s] %-44s (%s)\n", eng, src, msg ? msg : "?");
        return;
    }
    if (d == 0) {
        pass++;
        return;
    }
    fail++;
    char a[256], b[256];
    lisp_print(ro, a, sizeof(a));
    lisp_print(rv, b, sizeof(b));
    printf("  FAIL[%s] %s\n", eng, src);
    printf("        oracle %s%s  vm %s%s\n", oe ? "errored" : "=", oe ? "" : a,
           st == LBC_ERR ? "errored" : "=", st == LBC_ERR ? "" : b);
}

static void check(lisp_value genv, const char *src) {
    lisp_value expr = read1(src);
    const char *eo = NULL;
    lisp_value ro = lisp_eval(expr, genv, &eo);
    bool oe = (eo != NULL);

    lisp_value sv = LISP_UNDEF, rv = LISP_UNDEF;
    const char *sm = NULL, *rm = NULL;
    lbc_status ss = lbc_eval(genv, expr, &sv, &sm);
    lbc_status rs = rlbc_eval(genv, expr, &rv, &rm);
    run_engine("stack", src, ro, oe, ss, sv, sm);
    run_engine("reg", src, ro, oe, rs, rv, rm);
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
    "(let ((+ -)) (+ 5 3))",       // a local shadows the builtin -> no inline cache
    // flonum / mixed -> exact prim fallback
    "(+ 1.5 2.5)",
    "(< 1.5 2)",
    "(* 2.0 3)",
    // shared errors (both must error)
    "(car 5)",
    "(+ 1 'a)",
    // boxed cells: set! on a captured variable (counter closures)
    "(let ((c 0)) (let ((inc (lambda () (set! c (+ c 1)) c))) (inc) (inc) (inc)))",
    "(let ((n 0)) (let ((get (lambda () n)) (put (lambda (v) (set! n v)))) (put 42) (get)))",
    "(((lambda () (let ((n 0)) (lambda () (set! n (+ n 1)) n)))))",  // one-shot counter
    // independent captured cells per closure instance
    "(let ((mk (lambda () (let ((n 0)) (lambda () (set! n (+ n 1)) n)))))"
    "  (let ((f (mk)) (g (mk))) (+ (f) (f) (g))))",  // 1+2+1 = 4
    // letrec: mutual + forward local recursion
    "(letrec ((ev? (lambda (n) (if (= n 0) #t (od? (- n 1)))))"
    "         (od? (lambda (n) (if (= n 0) #f (ev? (- n 1)))))) (ev? 10))",
    "(letrec ((f (lambda (n) (if (< n 1) 0 (f (- n 1)))))) (f 3))",
    "(letrec ((a 1) (b (+ a 1))) (+ a b))",  // forward value reference (letrec*)
    // mutual recursion via internal defines (letrec* group)
    "(let () (define (ev n) (if (= n 0) #t (od (- n 1))))"
    "        (define (od n) (if (= n 0) #f (ev (- n 1)))) (ev 8))",
    // a deeper capture: closure over a loop variable, returned and called later
    "(let ((fs (let loop ((i 0) (acc '()))"
    "            (if (= i 3) acc (loop (+ i 1) (cons (lambda () i) acc))))))"
    "  (+ ((car fs)) ((car (cdr fs)))))",  // captured loop vars are distinct per frame
    // declines (run on oracle; reported, not failed)
    "(case 1 ((1) 'a) (else 'b))",
};

// --- Redefinition / deopt test ----------------------------------------------
// The fuzzer never rebinds an operator, so it cannot exercise the inline cache's
// deopt path -- the whole point of Decision 2 (core ops stay redefinable). This
// drives it directly: an op compiled to an IC must, when the operator is later
// rebound, transparently call the NEW binding (runtime guard fail), and a fresh
// compile after the rebind must not inline at all (compile-time deopt).
static int redef_fails = 0;
static void expect_eq(const char *what, lisp_value got, int want) {
    if (lisp_is_fixnum(got) && lisp_fixnum_val(got) == want)
        return;
    char b[128];
    lisp_print(got, b, sizeof(b));
    printf("  REDEF FAIL[%s]: got %s want %d\n", what, b, want);
    redef_fails++;
}

static void test_redefinition(lisp_value genv) {
    lisp_value plus = lisp_make_symbol("+", 1);
    lisp_value orig;
    if (!lisp_env_lookup(genv, plus, &orig))
        return;

    // 1) Compile (+ 2 3) while + is the canonical builtin: emits an IC. Running
    //    it now must give 5.
    bcchunk *k = NULL;
    const char *why = NULL;
    if (!rlbc_compile(genv, read1("(+ 2 3)"), &k, &why)) {
        printf("  REDEF FAIL: compile declined (%s)\n", why);
        redef_fails++;
        return;
    }
    if (k->nics < 1) {
        printf("  REDEF FAIL: expected an inline cache for (+ 2 3)\n");
        redef_fails++;
    }
    bcclosure *top = (bcclosure *)calloc(1, sizeof(bcclosure));
    top->chunk = k;
    lisp_value out = LISP_UNDEF;
    const char *err = NULL;
    rvm_run(top, genv, &out, &err);
    expect_eq("builtin +", out, 5);

    // 2) Rebind + to a closure computing a*b, then re-run the SAME chunk. The IC
    //    guard (cdr(cell) == expected) now fails, so OPCALL must call the new +.
    lisp_eval(read1("(set! + (lambda (a b) (* a b)))"), genv, &err);
    out = LISP_UNDEF; err = NULL;
    rvm_run(top, genv, &out, &err);
    expect_eq("runtime deopt", out, 6);

    // 3) A fresh compile while + is non-canonical must not inline (compile-time
    //    deopt) and still produce 6 via a normal call.
    bcchunk *k2 = NULL;
    if (rlbc_compile(genv, read1("(+ 2 3)"), &k2, &why)) {
        if (k2->nics != 0) {
            printf("  REDEF FAIL: inline cache emitted for a redefined op\n");
            redef_fails++;
        }
        bcclosure *t2 = (bcclosure *)calloc(1, sizeof(bcclosure));
        t2->chunk = k2;
        out = LISP_UNDEF; err = NULL;
        rvm_run(t2, genv, &out, &err);
        expect_eq("compile-time deopt", out, 6);
    }

    // Restore the canonical + so the bench / churn paths stay representative.
    lisp_env_set(genv, plus, orig);
    printf("\n=== redefinition / inline-cache deopt ===\n");
    printf("  %s\n", redef_fails == 0 ? "ok (runtime + compile-time deopt)" : "FAILED");
}

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
static lisp_value g_set, g_begin;  // set! / begin, for the mutate-captured path

static void fuzz_init(void) {
    const char *names[] = {"+", "-", "*", "<", "<=", ">", ">=", "=",
                           "if", "and", "or", "not", "let", "lambda"};
    for (int i = 0; i < 14; i++)
        g_ops[i] = lisp_make_symbol(names[i], strlen(names[i]));
    g_set = lisp_make_symbol("set!", 4);
    g_begin = lisp_make_symbol("begin", 5);
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

// Well-typed generation: gen_num always yields a numeric-valued expression and
// gen_bool a boolean-valued one, so the vast majority of fuzz cases produce
// real values to compare (deeply exercising nested closures/upvalues/control
// flow), while flonum literals still mix in to drive the prim fallback path.
// Introduced variables are numeric (used only in numeric positions).
static lisp_value gen_num(fscope *s, int depth);
static lisp_value gen_bool(fscope *s, int depth);

static lisp_value num_atom(fscope *s) {
    int k = (int)(rnd() % 10);
    if (s->nvars > 0 && k < 5)
        return s->vars[rnd() % (uint32_t)s->nvars];
    if (k < 6)
        return lisp_make_flonum((double)((int)(rnd() % 200) - 100) / 10.0);
    return lisp_fixnum((int)(rnd() % 41) - 20);
}

static lisp_value gen_num(fscope *s, int depth) {
    if (depth <= 0)
        return num_atom(s);
    lisp_value xs[4];
    switch ((int)(rnd() % 6)) {
        case 0:  // arithmetic
            xs[0] = g_ops[rnd() % 3];
            xs[1] = gen_num(s, depth - 1);
            xs[2] = gen_num(s, depth - 1);
            return lst(3, xs);
        case 1:  // (if BOOL NUM NUM)
            xs[0] = g_ops[OP_S_IF];
            xs[1] = gen_bool(s, depth - 1);
            xs[2] = gen_num(s, depth - 1);
            xs[3] = gen_num(s, depth - 1);
            return lst(4, xs);
        case 2: {  // (let ((v NUM)) NUM)
            if (s->nvars >= 15)
                return num_atom(s);
            lisp_value v = g_vpool[s->nvars];
            lisp_value init = gen_num(s, depth - 1);
            lisp_value binds =
                lisp_cons(lisp_cons(v, lisp_cons(init, LISP_EMPTY)), LISP_EMPTY);
            s->vars[s->nvars++] = v;
            lisp_value body = gen_num(s, depth - 1);
            s->nvars--;
            xs[0] = g_ops[OP_S_LET];
            xs[1] = binds;
            xs[2] = body;
            return lst(3, xs);
        }
        case 3: {  // ((lambda (v) NUM) NUM) -- closure + by-value capture
            if (s->nvars >= 15)
                return num_atom(s);
            lisp_value v = g_vpool[s->nvars];
            lisp_value arg = gen_num(s, depth - 1);
            s->vars[s->nvars++] = v;
            lisp_value body = gen_num(s, depth - 1);
            s->nvars--;
            lisp_value lam = lisp_cons(
                g_ops[OP_S_LAMBDA],
                lisp_cons(lisp_cons(v, LISP_EMPTY), lisp_cons(body, LISP_EMPTY)));
            xs[0] = lam;
            xs[1] = arg;
            return lst(2, xs);
        }
        case 4: {  // ((lambda () (set! v NUM) v)) -- mutate a CAPTURED var via cell
            if (s->nvars == 0)
                return num_atom(s);
            lisp_value v = s->vars[rnd() % (uint32_t)s->nvars];
            lisp_value setf = lst(3, (lisp_value[]){g_set, v, gen_num(s, depth - 1)});
            lisp_value body = lst(3, (lisp_value[]){g_begin, setf, v});
            lisp_value lam = lisp_cons(g_ops[OP_S_LAMBDA],
                                       lisp_cons(LISP_EMPTY, lisp_cons(body, LISP_EMPTY)));
            return lst(1, (lisp_value[]){lam});  // immediately applied, 0 args
        }
        default:
            return num_atom(s);
    }
}

static lisp_value gen_bool(fscope *s, int depth) {
    if (depth <= 0)
        return (rnd() & 1) ? LISP_TRUE : LISP_FALSE;
    lisp_value xs[4];
    switch ((int)(rnd() % 6)) {
        case 0:  // comparison of two numerics
            xs[0] = g_ops[OPC_LT + rnd() % 5];
            xs[1] = gen_num(s, depth - 1);
            xs[2] = gen_num(s, depth - 1);
            return lst(3, xs);
        case 1:  // and
            xs[0] = g_ops[OP_S_AND];
            xs[1] = gen_bool(s, depth - 1);
            xs[2] = gen_bool(s, depth - 1);
            return lst(3, xs);
        case 2:  // or
            xs[0] = g_ops[OP_S_OR];
            xs[1] = gen_bool(s, depth - 1);
            xs[2] = gen_bool(s, depth - 1);
            return lst(3, xs);
        case 3:  // not
            xs[0] = g_ops[OP_S_NOT];
            xs[1] = gen_bool(s, depth - 1);
            return lst(2, xs);
        case 4:  // (if BOOL BOOL BOOL)
            xs[0] = g_ops[OP_S_IF];
            xs[1] = gen_bool(s, depth - 1);
            xs[2] = gen_bool(s, depth - 1);
            xs[3] = gen_bool(s, depth - 1);
            return lst(4, xs);
        default:
            xs[0] = g_ops[OPC_LT + rnd() % 5];
            xs[1] = gen_num(s, depth - 1);
            xs[2] = gen_num(s, depth - 1);
            return lst(3, xs);
    }
}

static lisp_value gen(fscope *s, int depth) {
    return (rnd() & 1) ? gen_num(s, depth) : gen_bool(s, depth);
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
        bool oe = (eo != NULL);

        lisp_value sv = LISP_UNDEF, rv = LISP_UNDEF;
        const char *sm = NULL, *rm = NULL;
        lbc_status ss = lbc_eval(genv, expr, &sv, &sm);
        lbc_status rs = rlbc_eval(genv, expr, &rv, &rm);

        struct {
            const char *eng;
            lbc_status st;
            lisp_value v;
        } engs[2] = {{"stack", ss, sv}, {"reg", rs, rv}};
        for (int e = 0; e < 2; e++) {
            int d = diff_engine(ro, oe, engs[e].st, engs[e].v);
            if (d == 2) {
                dec++;
            } else if (d == 0) {
                if (oe)
                    both_err++;
                else
                    values++;
            } else {
                fails++;
                if (fails <= 8) {
                    char b[256];
                    lisp_print(expr, b, sizeof(b));
                    printf("  FUZZ FAIL[%s] %s\n", engs[e].eng, b);
                }
            }
        }
    }
    printf("\n=== fuzz: %d random exprs x 2 engines ===\n", iters);
    printf("  %d value-match, %d error-parity, %d declined, %d FAILED\n", values,
           both_err, dec, fails);
    return fails;
}

// Static instruction counts across a chunk tree (proxy for dispatch density).
static int count_stack_instrs(bcchunk *k) {
    int n = k->ncode;
    for (int i = 0; i < k->nchildren; i++)
        n += count_stack_instrs(k->children[i]);
    return n;
}
static int count_reg_instrs(bcchunk *k) {
    int n = k->nrcode;
    for (int i = 0; i < k->nchildren; i++)
        n += count_reg_instrs(k->children[i]);
    return n;
}

static void bench(lisp_value genv) {
    // A tail-recursive counting loop -- the hot shape (named-let, frozen ops).
    const char *src =
        "(let loop ((i 0) (acc 0)) (if (= i 2000000) acc (loop (+ i 1) (+ acc 1))))";
    lisp_value expr = read1(src);

    bcchunk *ks = NULL, *kr = NULL;
    const char *why = NULL;
    if (!lbc_compile(genv, expr, &ks, &why) || !rlbc_compile(genv, expr, &kr, &why)) {
        printf("\n[bench] compile declined: %s\n", why);
        return;
    }
    bcclosure *tops = (bcclosure *)calloc(1, sizeof(bcclosure));
    bcclosure *topr = (bcclosure *)calloc(1, sizeof(bcclosure));
    tops->chunk = ks;
    topr->chunk = kr;

    lisp_value sv = LISP_UNDEF, rv = LISP_UNDEF, ro = LISP_UNDEF;
    const char *err = NULL, *eo = NULL;
    vm_run(tops, genv, &sv, &err);
    rvm_run(topr, genv, &rv, &err);  // warm + correctness
    ro = lisp_eval(expr, genv, &eo);

    double t0 = now_sec();
    vm_run(tops, genv, &sv, &err);
    double t_stk = now_sec() - t0;
    t0 = now_sec();
    rvm_run(topr, genv, &rv, &err);
    double t_reg = now_sec() - t0;
    t0 = now_sec();
    (void)lisp_eval(expr, genv, &eo);
    double t_or = now_sec() - t0;

    printf("\n=== bench: 2,000,000-iteration tail loop ===\n");
    printf("  results: stack=%s reg=%s oracle %s\n",
           vequal(sv, ro) ? "ok" : "BAD", vequal(rv, ro) ? "ok" : "BAD",
           (vequal(sv, ro) && vequal(rv, ro)) ? "(match)" : "(MISMATCH)");
    printf("  static instrs (chunk tree):  stack %d  ->  register %d\n",
           count_stack_instrs(ks), count_reg_instrs(kr));
    printf("  tree-walker   %8.2f ms\n", t_or * 1000);
    printf("  stack VM      %8.2f ms   %6.2fx vs tree-walker\n", t_stk * 1000,
           t_or / t_stk);
    printf("  register VM   %8.2f ms   %6.2fx vs tree-walker   %5.2fx vs stack VM\n",
           t_reg * 1000, t_or / t_reg, t_stk / t_reg);
}

static bcclosure *compile_top(lisp_value genv, const char *src) {
    lisp_value e = read1(src);
    bcchunk *k = NULL;
    const char *why = NULL;
    if (!lbc_compile(genv, e, &k, &why)) {
        printf("compile declined: %s\n", why);
        exit(3);
    }
    bcclosure *top = (bcclosure *)calloc(1, sizeof(bcclosure));
    top->chunk = k;
    return top;
}

static double time_run(bcclosure *top, lisp_value genv, lisp_value *out) {
    const char *err = NULL;
    double t0 = now_sec();
    vm_run(top, genv, out, &err);
    return now_sec() - t0;
}

// Quantify the "C ABI churn": compare an INLINED arithmetic loop against the same
// loop where the add is forced through the call path (3-arg +, which the compiler
// routes as LOADGLOBAL + CALL), under both the thin direct->fn convention and the
// heavyweight lisp_apply path. EN iterations, 2 adds/iter.
#define EN 2000000
static void churn_experiment(lisp_value genv) {
    const char *A =  // both adds inlined (2-arg frozen op)
        "(let loop ((i 0) (acc 0)) (if (= i 2000000) acc (loop (+ i 1) (+ acc 1))))";
    const char *B =  // both adds via the call path (3-arg, not a frozen opcode)
        "(let loop ((i 0) (acc 0)) (if (= i 2000000) acc (loop (+ i 1 0) (+ acc 1 0))))";
    // Compile the call-path loop B in two ways: globals resolved to slots, and
    // globals resolved by per-call hash lookup.
    g_global_slots = 1;
    bcclosure *ta = compile_top(genv, A);
    bcclosure *tb_slot = compile_top(genv, B);
    g_global_slots = 0;
    bcclosure *tb_hash = compile_top(genv, B);
    g_global_slots = 1;
    lisp_value o = LISP_UNDEF;

    time_run(ta, genv, &o);  // warm
    time_run(tb_slot, genv, &o);

    g_thin_prim = 1;
    double t_inline = time_run(ta, genv, &o);
    double t_thin_slot = time_run(tb_slot, genv, &o);
    double t_thin_hash = time_run(tb_hash, genv, &o);
    g_thin_prim = 0;
    double t_apply_hash = time_run(tb_hash, genv, &o);
    g_thin_prim = 1;

    double per = 2.0 * (double)EN;  // primitive ops per loop
    printf("\n=== churn experiment: inline / call-path / ABI / global-slots ===\n");
    printf("  (2,000,000 iters, 2 adds/iter)\n");
    printf("  inlined opcode (+ a b)             %8.2f ms   %5.1f ns/add\n",
           t_inline * 1000, t_inline * 1e9 / per);
    printf("  call: lisp_apply + global hash     %8.2f ms   %5.1f ns/add\n",
           t_apply_hash * 1000, t_apply_hash * 1e9 / per);
    printf("  call: thin ->fn   + global hash    %8.2f ms   %5.1f ns/add\n",
           t_thin_hash * 1000, t_thin_hash * 1e9 / per);
    printf("  call: thin ->fn   + global slot    %8.2f ms   %5.1f ns/add\n",
           t_thin_slot * 1000, t_thin_slot * 1e9 / per);
    printf("  --\n");
    printf("  thin ->fn vs lisp_apply (hash):     %.2fx  (-%.1f ns/add)\n",
           t_apply_hash / t_thin_hash, (t_apply_hash - t_thin_hash) * 1e9 / per);
    printf("  global slot vs hash (thin ->fn):    %.2fx  (-%.1f ns/add)\n",
           t_thin_hash / t_thin_slot, (t_thin_hash - t_thin_slot) * 1e9 / per);
    printf("  combined (thin+slot vs apply+hash): %.2fx\n",
           t_apply_hash / t_thin_slot);
    printf("  residual call overhead over inlined: %.1f ns/add\n",
           (t_thin_slot - t_inline) * 1e9 / per);
}

int main(void) {
    lisp_set_output(host_out, NULL);
    lisp_value genv = lisp_default_env();

    printf("=== bytecode backend vs tree-walker (differential) ===\n");
    for (size_t i = 0; i < sizeof(CORPUS) / sizeof(CORPUS[0]); i++)
        check(genv, CORPUS[i]);

    printf("\n  %d passed, %d declined, %d FAILED\n", pass, declined, fail);

    test_redefinition(genv);
    int fuzz_fails = fuzz(genv, 20000);
    bench(genv);
    churn_experiment(genv);

    int total_fail = fail + fuzz_fails + redef_fails;
    printf("\n[bytecode] %s\n", total_fail == 0 ? "ALL TESTS PASSED" : "FAILURES PRESENT");
    return total_fail == 0 ? 0 : 1;
}
