// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// Host-built test: escape-only call/cc, exceptions (raise/guard/error/
// with-exception-handler), and multiple values.

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
        printf("  FAIL %-52s -> error: %s\n", src, err);
        failures++;
        return;
    }
    char buf[256];
    lisp_print(v, buf, sizeof(buf));
    if (strcmp(buf, expect) != 0) {
        printf("  FAIL %-52s -> got '%s' want '%s'\n", src, buf, expect);
        failures++;
    } else {
        printf("  ok   %-52s -> %s\n", src, buf);
    }
}

int main(void) {
    printf("[lisp control] call/cc + exceptions + values\n");

    // call/cc: normal return (continuation not used)
    evals("(call/cc (lambda (k) 42))", "42");
    // call/cc: escape with a value
    evals("(+ 1 (call/cc (lambda (k) (k 10) 999)))", "11");
    // escape out of a deep computation
    evals("(call/cc (lambda (return)"
          "  (for-each (lambda (x) (if (= x 3) (return x))) '(1 2 3 4 5))"
          "  'not-found))",
          "3");
    // call/cc as a non-local exit from a fold/recursion (product, short-circuit on 0)
    evals("(define (product lst)"
          "  (call/cc (lambda (break)"
          "    (let loop ((l lst) (acc 1))"
          "      (cond ((null? l) acc)"
          "            ((= (car l) 0) (break 0))"
          "            (else (loop (cdr l) (* acc (car l)))))))))"
          " (product '(1 2 3 0 4 5))",
          "0");
    evals("(define (product lst)"
          "  (call/cc (lambda (break)"
          "    (let loop ((l lst) (acc 1))"
          "      (cond ((null? l) acc)"
          "            ((= (car l) 0) (break 0))"
          "            (else (loop (cdr l) (* acc (car l)))))))))"
          " (product '(1 2 3 4))",
          "24");

    // raise + guard
    evals("(guard (e (#t (list 'caught e))) (raise 'boom))", "(caught boom)");
    evals("(guard (e (#t 'caught)) 'no-raise)", "no-raise");
    // guard with cond-style clauses + else
    evals("(guard (e ((symbol? e) 'was-symbol) (else 'other)) (raise 'sym))", "was-symbol");
    evals("(guard (e ((symbol? e) 'was-symbol) (else 'other)) (raise 42))", "other");
    // re-raise when no clause matches, caught by an outer guard
    evals("(guard (e (#t (list 'outer e)))"
          "  (guard (e ((number? e) 'num)) (raise 'sym)))",
          "(outer sym)");

    // error + error-object accessors, caught by guard
    evals("(guard (e ((error-object? e) (error-object-message e)))"
          "  (error \"bad thing\" 1 2))",
          "\"bad thing\"");
    evals("(guard (e ((error-object? e) (error-object-irritants e)))"
          "  (error \"x\" 1 2 3))",
          "(1 2 3)");

    // guard also catches a plain interpreter error (wrapped as an error object)
    evals("(guard (e (#t 'caught-the-car-error)) (car '()))", "caught-the-car-error");

    // with-exception-handler
    evals("(with-exception-handler"
          "  (lambda (e) (list 'handled e))"
          "  (lambda () (raise 'oops)))",
          "(handled oops)");

    // values + call-with-values
    evals("(call-with-values (lambda () (values 1 2 3)) list)", "(1 2 3)");
    evals("(call-with-values (lambda () (values 1 2 3)) +)", "6");
    evals("(call-with-values (lambda () 42) list)", "(42)");

    // a continuation used to implement a generator-style early return inside map
    evals("(call/cc (lambda (k)"
          "  (map (lambda (x) (if (negative? x) (k 'neg) x)) '(1 2 -3 4))))",
          "neg");

    printf("\n[lisp control] %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
