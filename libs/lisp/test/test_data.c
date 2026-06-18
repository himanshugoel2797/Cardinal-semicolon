// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// Host-built Phase 2 test: interning, vectors, equal?, predicates, the list
// library, cond/and/or, and higher-order procedures.

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
    char buf[512];
    lisp_print(v, buf, sizeof(buf));
    if (strcmp(buf, expect) != 0) {
        printf("  FAIL %-46s -> got '%s' want '%s'\n", src, buf, expect);
        failures++;
    } else {
        printf("  ok   %-46s -> %s\n", src, buf);
    }
}

static void evalerr(const char *src) {
    checks++;
    lisp_value env = lisp_default_env();
    const char *err = NULL;
    lisp_value v = lisp_eval_string(src, env, &err);
    if (v == LISP_UNDEF && err != NULL) {
        printf("  ok   %-46s -> (error: %s)\n", src, err);
    } else {
        char buf[256];
        lisp_print(v, buf, sizeof(buf));
        printf("  FAIL %-46s -> expected error, got '%s'\n", src, buf);
        failures++;
    }
}

int main(void) {
    printf("[lisp Phase 2] data types + library\n");

    // Symbol interning -> eq? identity
    evals("(eq? 'a 'a)", "#t");
    evals("(eq? 'a 'b)", "#f");
    evals("(eq? (car '(x y)) 'x)", "#t");
    evals("(define s 'hello) (eq? s 'hello)", "#t");

    // equal? deep structural
    evals("(equal? '(1 2 3) '(1 2 3))", "#t");
    evals("(equal? '(1 (2 3)) '(1 (2 3)))", "#t");
    evals("(equal? '(1 2) '(1 2 3))", "#f");
    evals("(eq? '(1 2) '(1 2))", "#f");  // distinct pairs
    evals("'(1 . 2)", "(1 . 2)");        // dotted pair reads + prints
    evals("'(1 2 . 3)", "(1 2 . 3)");
    evals("(cdr '(1 . 2))", "2");
    evals("(equal? \"abc\" \"abc\")", "#t");
    evals("(equal? #(1 2 3) #(1 2 3))", "#t");
    evals("(equal? #(1 2) #(1 9))", "#f");

    // Type predicates
    evals("(symbol? 'x)", "#t");
    evals("(symbol? 5)", "#f");
    evals("(integer? 5)", "#t");
    evals("(number? 5)", "#t");
    evals("(boolean? #t)", "#t");
    evals("(boolean? 0)", "#f");
    evals("(string? \"hi\")", "#t");
    evals("(char? #\\a)", "#t");
    evals("(vector? #(1 2))", "#t");
    evals("(pair? '(1))", "#t");
    evals("(null? '())", "#t");
    evals("(procedure? car)", "#t");
    evals("(procedure? (lambda (x) x))", "#t");
    evals("(procedure? 5)", "#f");

    // List library
    evals("(length '(a b c d))", "4");
    evals("(length '())", "0");
    evals("(reverse '(1 2 3))", "(3 2 1)");
    evals("(append '(1 2) '(3 4))", "(1 2 3 4)");
    evals("(append '(1) '(2) '(3))", "(1 2 3)");
    evals("(append)", "()");
    evals("(append '() '(1))", "(1)");
    evals("(list-ref '(a b c) 0)", "a");
    evals("(list-ref '(a b c) 2)", "c");

    // Vectors
    evals("#(1 2 3)", "#(1 2 3)");
    evals("#()", "#()");
    evals("(vector 1 2 3)", "#(1 2 3)");
    evals("(vector-ref #(10 20 30) 1)", "20");
    evals("(vector-length #(1 2 3 4))", "4");
    evals("(make-vector 3 0)", "#(0 0 0)");
    evals("(vector->list #(1 2 3))", "(1 2 3)");
    evals("(list->vector '(a b c))", "#(a b c)");
    evals("#(1 (2 3) #(4 5))", "#(1 (2 3) #(4 5))");

    // cond / and / or
    evals("(cond (#f 1) (#t 2) (else 3))", "2");
    evals("(cond (#f 1) (else 'fallback))", "fallback");
    evals("(cond (#f 1) (#f 2))", "#<undef>");  // no clause matched -> unspecified
    evals("(cond (42))", "42");                 // test value when no body
    evals("(and 1 2 3)", "3");
    evals("(and 1 #f 3)", "#f");
    evals("(and)", "#t");
    evals("(or #f #f 7)", "7");
    evals("(or #f #f)", "#f");
    evals("(or)", "#f");
    evals("(and (< 1 2) (< 2 3))", "#t");

    // Higher-order
    evals("(map (lambda (x) (* x x)) '(1 2 3 4))", "(1 4 9 16)");
    evals("(map car '((1 2) (3 4) (5 6)))", "(1 3 5)");
    evals("(apply + '(1 2 3 4))", "10");
    evals("(apply + 1 2 '(3 4))", "10");
    evals("(for-each (lambda (x) x) '(1 2 3))", "#<undef>");  // returns unspecified

    // Large lists must not overflow the (kernel) stack: map/append/equal? are all
    // iterative. iota uses a tail-recursive define (handled by the eval loop).
    const char *iota = "(define (iota i acc) (if (= i 0) acc (iota (- i 1) (cons i acc)))) ";
    char prog[512];
    snprintf(prog, sizeof(prog), "%s(length (map (lambda (x) (* x x)) (iota 5000 '())))", iota);
    evals(prog, "5000");
    snprintf(prog, sizeof(prog), "%s(length (append (iota 3000 '()) (iota 3000 '())))", iota);
    evals(prog, "6000");
    snprintf(prog, sizeof(prog), "%s(equal? (iota 4000 '()) (iota 4000 '()))", iota);
    evals(prog, "#t");

    // Combined: a small program
    evals("(define (sum lst) (if (null? lst) 0 (+ (car lst) (sum (cdr lst)))))"
          " (sum (map (lambda (x) (* x 2)) '(1 2 3 4 5)))",
          "30");

    // Errors
    evalerr("(vector-ref #(1 2) 5)");
    evalerr("(vector-ref #(1 2) -1)");
    evalerr("(car 5)");
    evalerr("(length '(1 2 . 3))");
    evalerr("(list-ref '(a b) 10)");
    evalerr("(make-vector -1)");
    evalerr("#(1 . 2)");  // improper vector literal is a read error

    printf("\n[lisp Phase 2] %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
