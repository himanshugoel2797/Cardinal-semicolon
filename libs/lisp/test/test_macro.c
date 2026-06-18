// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// Host-built test: syntax-rules macros (define-syntax), incl. ellipsis.

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
        printf("  FAIL %-30s -> error: %s\n", "(case)", err);
        failures++;
        return;
    }
    char buf[256];
    lisp_print(v, buf, sizeof(buf));
    if (strcmp(buf, expect) != 0) {
        printf("  FAIL -> got '%s' want '%s'\n", buf, expect);
        failures++;
    } else {
        printf("  ok   -> %s\n", buf);
    }
}

int main(void) {
    printf("[lisp macros] syntax-rules\n");

    // Simple, no ellipsis
    evals("(define-syntax my-if (syntax-rules () ((_ c t e) (cond (c t) (else e)))))"
          " (my-if #t 'yes 'no)",
          "yes");

    // when / unless with body ellipsis
    evals("(define-syntax when (syntax-rules () ((_ t body ...) (if t (begin body ...) #f))))"
          " (when #t 1 2 3)",
          "3");
    evals("(define-syntax when (syntax-rules () ((_ t body ...) (if t (begin body ...) #f))))"
          " (when #f 1 2 3)",
          "#f");
    evals("(define-syntax unless (syntax-rules () ((_ t body ...) (if t #f (begin body ...)))))"
          " (unless #f 'ran)",
          "ran");

    // swap! using a temporary (hygiene-lite: tmp is introduced)
    evals("(define-syntax swap! (syntax-rules ()"
          "  ((_ a b) (let ((tmp a)) (set! a b) (set! b tmp)))))"
          " (define x 1) (define y 2) (swap! x y) (list x y)",
          "(2 1)");

    // ellipsis: variadic list builder
    evals("(define-syntax my-list (syntax-rules () ((_ x ...) (list x ...))))"
          " (my-list 1 2 3 4)",
          "(1 2 3 4)");
    evals("(define-syntax my-list (syntax-rules () ((_ x ...) (list x ...))))"
          " (my-list)",
          "()");

    // recursive macro with ellipsis: my-or
    evals("(define-syntax my-or (syntax-rules ()"
          "  ((_) #f)"
          "  ((_ e) e)"
          "  ((_ e1 e2 ...) (let ((t e1)) (if t t (my-or e2 ...))))))"
          " (my-or #f #f 7)",
          "7");
    evals("(define-syntax my-or (syntax-rules ()"
          "  ((_) #f) ((_ e) e)"
          "  ((_ e1 e2 ...) (let ((t e1)) (if t t (my-or e2 ...))))))"
          " (my-or)",
          "#f");

    // two parallel ellipsis groups: a let implemented as a macro
    evals("(define-syntax my-let (syntax-rules ()"
          "  ((_ ((name val) ...) body ...)"
          "   ((lambda (name ...) body ...) val ...))))"
          " (my-let ((a 1) (b 2) (c 3)) (+ a b c))",
          "6");

    // literal matching
    evals("(define-syntax classify (syntax-rules (zero)"
          "  ((_ zero) 'got-zero)"
          "  ((_ x) 'got-other)))"
          " (list (classify zero) (classify 5))",
          "(got-zero got-other)");

    // nested ellipsis: flatten one level of grouping
    evals("(define-syntax pairs (syntax-rules ()"
          "  ((_ (a b) ...) (list (list a ...) (list b ...)))))"
          " (pairs (1 2) (3 4) (5 6))",
          "((1 3 5) (2 4 6))");

    // a while loop built from a recursive helper macro
    evals("(define-syntax while (syntax-rules ()"
          "  ((_ cond body ...)"
          "   (let loop () (if cond (begin body ... (loop)) #f)))))"
          " (define i 0) (define sum 0)"
          " (while (< i 5) (set! sum (+ sum i)) (set! i (+ i 1)))"
          " sum",
          "10");

    // Two vars under ONE template ellipsis, bound to sequences of different
    // lengths -> error (R7RS), not silent truncation.
    {
        checks++;
        lisp_value env = lisp_default_env();
        const char *err = NULL;
        lisp_eval_string("(define-syntax zip (syntax-rules ()"
                         "  ((_ (a ...) (b ...)) (list (cons a b) ...))))",
                         env, &err);
        err = NULL;
        lisp_value v = lisp_eval_string("(zip (1 2 3) (9))", env, &err);
        if (v == LISP_UNDEF && err != NULL)
            printf("  ok   mismatched-ellipsis errors -> %s\n", err);
        else {
            char b[64];
            lisp_print(v, b, sizeof(b));
            printf("  FAIL mismatched ellipsis: expected error, got %s\n", b);
            failures++;
        }
        // equal lengths zip fine; independent ellipses (a ... b ...) also fine.
        evals("(define-syntax zip2 (syntax-rules ()"
              "  ((_ (a ...) (b ...)) (list (cons a b) ...))))"
              " (zip2 (1 2) (3 4))",
              "((1 . 3) (2 . 4))");
        evals("(define-syntax cat (syntax-rules ()"
              "  ((_ (a ...) (b ...)) (list a ... b ...))))"
              " (cat (1 2 3) (9))",
              "(1 2 3 9)");
    }

    // (syntax-rules) with no literals list must error, not crash.
    {
        checks++;
        lisp_value env = lisp_default_env();
        const char *err = NULL;
        lisp_value v = lisp_eval_string("(define-syntax bad (syntax-rules))", env, &err);
        if (v == LISP_UNDEF && err != NULL)
            printf("  ok   (syntax-rules) errors cleanly -> %s\n", err);
        else {
            printf("  FAIL (syntax-rules) should error\n");
            failures++;
        }
    }

    printf("\n[lisp macros] %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
