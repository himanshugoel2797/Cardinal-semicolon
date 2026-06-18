// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// Host-built Phase 3a test: binding forms (let*, letrec, named let) and
// quasiquote / unquote / unquote-splicing.

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
        printf("  FAIL %-50s -> error: %s\n", src, err);
        failures++;
        return;
    }
    char buf[512];
    lisp_print(v, buf, sizeof(buf));
    if (strcmp(buf, expect) != 0) {
        printf("  FAIL %-50s -> got '%s' want '%s'\n", src, buf, expect);
        failures++;
    } else {
        printf("  ok   %-50s -> %s\n", src, buf);
    }
}

int main(void) {
    printf("[lisp Phase 3a] binding forms + quasiquote\n");

    // let*
    evals("(let* ((a 1) (b (+ a 1)) (c (+ a b))) (list a b c))", "(1 2 3)");
    evals("(let* ((x 10)) x)", "10");
    evals("(let* () 42)", "42");

    // letrec: mutual recursion
    evals("(letrec ((even? (lambda (n) (if (= n 0) #t (odd? (- n 1)))))"
          "         (odd?  (lambda (n) (if (= n 0) #f (even? (- n 1))))))"
          "  (list (even? 10) (odd? 7)))",
          "(#t #t)");

    // named let
    evals("(let loop ((i 0) (acc 0)) (if (= i 5) acc (loop (+ i 1) (+ acc i))))", "10");
    evals("(let build ((n 5) (acc '())) (if (= n 0) acc (build (- n 1) (cons n acc))))",
          "(1 2 3 4 5)");
    // named let must tail-call (no stack growth)
    evals("(let loop ((i 1000000)) (if (= i 0) 'done (loop (- i 1))))", "done");

    // quasiquote
    evals("`(1 2 3)", "(1 2 3)");
    evals("`()", "()");
    evals("(define x 5) `(a ,x c)", "(a 5 c)");
    evals("`(1 ,(+ 2 3) 4)", "(1 5 4)");
    evals("(define xs '(1 2 3)) `(a ,@xs b)", "(a 1 2 3 b)");
    evals("`(,@(list 1 2) ,@(list 3 4))", "(1 2 3 4)");
    evals("(define n 7) `,n", "7");
    evals("`(a (b ,(+ 1 2)))", "(a (b 3))");
    // splice at the end
    evals("(define xs '(2 3)) `(1 ,@xs)", "(1 2 3)");
    // nested quasiquote: inner unquote is NOT evaluated at the outer level
    evals("`(a `(b ,(+ 1 2)))", "(a (quasiquote (b (unquote (+ 1 2)))))");

    // combined: build code-like data
    evals("(define (make-adder n) `(lambda (x) (+ x ,n))) (make-adder 5)",
          "(lambda (x) (+ x 5))");

    printf("\n[lisp Phase 3a] %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
