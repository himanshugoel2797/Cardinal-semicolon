// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// Host-built Phase 1 test: evaluate Scheme programs and check the printed result.

#include <stdio.h>
#include <string.h>

#include "lisp.h"

static int failures = 0;
static int checks = 0;

// Evaluate all forms in `src` in a fresh default env; assert printed result.
static void evals(const char *src, const char *expect) {
    checks++;
    lisp_value env = lisp_default_env();
    const char *err = NULL;
    lisp_value v = lisp_eval_string(src, env, &err);
    if (v == LISP_UNDEF && err != NULL) {
        printf("  FAIL %-40s -> error: %s\n", src, err);
        failures++;
        return;
    }
    char buf[512];
    lisp_print(v, buf, sizeof(buf));
    if (strcmp(buf, expect) != 0) {
        printf("  FAIL %-40s -> got '%s' want '%s'\n", src, buf, expect);
        failures++;
    } else {
        printf("  ok   %-40s -> %s\n", src, buf);
    }
}

// Assert that evaluating `src` raises an error (any).
static void evalerr(const char *src) {
    checks++;
    lisp_value env = lisp_default_env();
    const char *err = NULL;
    lisp_value v = lisp_eval_string(src, env, &err);
    if (v == LISP_UNDEF && err != NULL) {
        printf("  ok   %-40s -> (error: %s)\n", src, err);
    } else {
        char buf[256];
        lisp_print(v, buf, sizeof(buf));
        printf("  FAIL %-40s -> expected error, got '%s'\n", src, buf);
        failures++;
    }
}

int main(void) {
    // The evaluator is now an explicit-stack machine: a continuation frame is
    // allocated per reduction, so an unbounded tail loop (below) relies on the
    // GC to reclaim the per-iteration garbage. Enable it (the real runtime
    // always runs with the collector on).
    uintptr_t stack_marker;
    lisp_gc_init(&stack_marker);

    printf("[lisp Phase 1] evaluator\n");

    // Self-evaluating + arithmetic
    evals("42", "42");
    evals("#t", "#t");
    evals("(+ 1 2 3)", "6");
    evals("(- 10 3 2)", "5");
    evals("(- 5)", "-5");
    evals("(* 2 3 4)", "24");
    evals("(/ 20 2 5)", "2");
    evals("(modulo 17 5)", "2");
    evals("(+ (* 2 3) (- 10 4))", "12");

    // Comparison + booleans
    evals("(< 1 2 3)", "#t");
    evals("(< 1 3 2)", "#f");
    evals("(= 7 7)", "#t");
    evals("(< 5)", "#t");  // unary comparison is vacuously #t (R7RS)
    evals("(not #f)", "#t");
    evals("(not 0)", "#f");  // Scheme: 0 is truthy
    evals("(zero? 0)", "#t");

    // quote, pairs, lists
    evals("'foo", "foo");
    evals("'(1 2 3)", "(1 2 3)");
    evals("(cons 1 2)", "(1 . 2)");
    evals("(car '(a b c))", "a");
    evals("(cdr '(a b c))", "(b c)");
    evals("(list 1 2 3)", "(1 2 3)");
    evals("(null? '())", "#t");
    evals("(pair? '(1))", "#t");
    evals("(eq? 'a 'a)", "#t");

    // if
    evals("(if #t 1 2)", "1");
    evals("(if #f 1 2)", "2");
    evals("(if (< 1 2) 'yes 'no)", "yes");

    // define + lambda + closures
    evals("(define x 10) (+ x 5)", "15");
    evals("(define (square n) (* n n)) (square 9)", "81");
    evals("(define (add a b) (+ a b)) (add 3 4)", "7");
    evals("((lambda (x) (* x x)) 6)", "36");
    evals("(define inc (lambda (n) (+ n 1))) (inc 41)", "42");

    // let
    evals("(let ((a 2) (b 3)) (+ a b))", "5");
    evals("(let ((x 5)) (let ((y 10)) (+ x y)))", "15");

    // begin + set!
    evals("(define c 0) (set! c 7) c", "7");
    evals("(begin 1 2 3)", "3");

    // closure captures environment
    evals("(define (adder n) (lambda (x) (+ x n)))"
          " (define add5 (adder 5))"
          " (add5 100)",
          "105");

    // recursion
    evals("(define (fact n) (if (= n 0) 1 (* n (fact (- n 1))))) (fact 5)", "120");
    evals("(define (fib n) (if (< n 2) n (+ (fib (- n 1)) (fib (- n 2))))) (fib 10)", "55");

    // proper tail calls: a deep tail-recursive countdown must not overflow.
    evals("(define (loop n) (if (= n 0) 'done (loop (- n 1)))) (loop 1000000)", "done");

    // Errors
    evalerr("(car 5)");
    evalerr("undefined-variable");
    evalerr("(+ 1 'a)");
    evalerr("()");
    evalerr("(set! never-defined 3)");
    evalerr("(define x)");          // missing value expression
    evalerr("(define 5 10)");       // non-symbol define target
    evalerr("(define (5 a) a)");    // non-symbol function name

    printf("\n[lisp Phase 1] %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
