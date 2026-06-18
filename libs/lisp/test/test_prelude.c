// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// Host-built Phase 3d test: variadic lambda, the `error` primitive, and the
// Scheme-defined standard prelude (prelude.c).

#include <stdio.h>
#include <string.h>

#include "lisp.h"

static int failures = 0;
static int checks = 0;

static void evals(const char *src, const char *expect) {
    checks++;
    lisp_value env = lisp_default_env();
    const char *err = NULL;
    lisp_value v = lisp_eval_string(src, env, &err);
    if (v == LISP_UNDEF && err != NULL) {
        printf("  FAIL %-46s -> error: %s\n", src, err);
        failures++;
        return;
    }
    char buf[256];
    lisp_print(v, buf, sizeof(buf));
    if (strcmp(buf, expect) != 0) {
        printf("  FAIL %-46s -> got '%s' want '%s'\n", src, buf, expect);
        failures++;
    } else {
        printf("  ok   %-46s -> %s\n", src, buf);
    }
}

static void evalerr(const char *src, const char *want_msg) {
    checks++;
    lisp_value env = lisp_default_env();
    const char *err = NULL;
    lisp_value v = lisp_eval_string(src, env, &err);
    if (v == LISP_UNDEF && err != NULL &&
        (want_msg == NULL || strcmp(err, want_msg) == 0)) {
        printf("  ok   %-46s -> (error: %s)\n", src, err);
    } else {
        printf("  FAIL %-46s -> expected error '%s'\n", src, want_msg ? want_msg : "(any)");
        failures++;
    }
}

int main(void) {
    printf("[lisp Phase 3d] varargs + error + prelude\n");

    // Variadic lambda
    evals("((lambda args args) 1 2 3)", "(1 2 3)");
    evals("((lambda args args))", "()");
    evals("((lambda (a b . rest) (list a b rest)) 1 2 3 4 5)", "(1 2 (3 4 5))");
    evals("((lambda (a . rest) rest) 1)", "()");
    evals("(define (variadic-sum . ns) (fold-left + 0 ns)) (variadic-sum 1 2 3 4)", "10");

    // error primitive
    evalerr("(error \"boom\")", "boom");
    evalerr("(error \"bad thing\" 1 2)", "bad thing");

    // accessors
    evals("(cadr '(1 2 3))", "2");
    evals("(caddr '(1 2 3))", "3");
    evals("(cddr '(1 2 3 4))", "(3 4)");

    // numeric helpers
    evals("(abs -5)", "5");
    evals("(abs 5)", "5");
    evals("(even? 4)", "#t");
    evals("(odd? 7)", "#t");
    evals("(positive? 3)", "#t");
    evals("(negative? -2)", "#t");
    evals("(max 3 7 2 9 1)", "9");
    evals("(min 3 7 2 9 1)", "1");
    evals("(expt 2 10)", "1024");
    evals("(gcd 12 18)", "6");
    evals("(gcd 12 18 24)", "6");
    evals("(gcd -12 18)", "6");          // abs applied -> positive gcd
    // modulo (sign of divisor) vs remainder (sign of dividend), R7RS
    evals("(modulo 7 3)", "1");
    evals("(modulo -7 3)", "2");
    evals("(modulo 7 -3)", "-2");
    evals("(remainder -7 3)", "-1");
    evals("(remainder 7 -3)", "1");
    evals("(quotient 7 2)", "3");
    evals("(quotient -7 2)", "-3");
    evals("(add1 41)", "42");
    evals("(sub1 1)", "0");

    // list utilities
    evals("(list? '(1 2 3))", "#t");
    evals("(list? '(1 . 2))", "#f");
    evals("(list? 5)", "#f");
    evals("(list-tail '(a b c d) 2)", "(c d)");
    evals("(fold-left + 0 '(1 2 3 4 5))", "15");
    evals("(fold-left cons '() '(1 2 3))", "(((() . 1) . 2) . 3)");
    evals("(fold-right cons '() '(1 2 3))", "(1 2 3)");
    evals("(reduce + 0 '(1 2 3 4))", "10");
    evals("(reduce + 0 '())", "0");
    evals("(filter even? '(1 2 3 4 5 6))", "(2 4 6)");
    evals("(filter positive? '(-1 2 -3 4))", "(2 4)");
    evals("(assq 'b '((a . 1) (b . 2) (c . 3)))", "(b . 2)");
    evals("(assq 'z '((a . 1)))", "#f");
    evals("(assoc \"k\" '((\"j\" . 1) (\"k\" . 2)))", "(\"k\" . 2)");
    evals("(memq 'c '(a b c d))", "(c d)");
    evals("(memq 'z '(a b))", "#f");
    evals("(member 2 '(1 2 3))", "(2 3)");
    evals("(list-copy '(1 2 3))", "(1 2 3)");

    // composed: tail-recursive fold over a large list must not overflow
    evals("(define (iota i acc) (if (= i 0) acc (iota (- i 1) (cons i acc))))"
          " (fold-left + 0 (iota 5000 '()))",
          "12502500");

    printf("\n[lisp Phase 3d] %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
