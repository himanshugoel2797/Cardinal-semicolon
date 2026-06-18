// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// The standard library, written in Scheme and evaluated into the default
// environment at startup. Defining the library in the language itself (rather
// than in C) keeps the C core small and is the natural Lisp idiom. Everything
// here is built only from the primitives in prims.c.
//
// Stack discipline: the tree-walker gives proper tail calls, so tail-recursive
// definitions (assq, memq, list-tail, fold-left, the named-let loops) run in
// constant C stack. Non-tail list recursion is avoided (filter/fold-right go
// through fold-left + reverse) so these stay safe on the small kernel stack.

#include <stdint.h>  // before lisp.h: matches the other TUs and avoids a
                     // common/types.h double-include macro-redefinition

#include "lisp.h"

static const char *PRELUDE =
    // --- c[ad]+r accessors ---
    "(define (caar p) (car (car p)))"
    "(define (cadr p) (car (cdr p)))"
    "(define (cdar p) (cdr (car p)))"
    "(define (cddr p) (cdr (cdr p)))"
    "(define (caddr p) (car (cddr p)))"
    "(define (cdddr p) (cdr (cddr p)))"
    "(define (cadddr p) (car (cdddr p)))"
    "(define (first p) (car p))"
    "(define (second p) (cadr p))"
    "(define (third p) (caddr p))"

    // --- numeric helpers ---
    "(define (add1 n) (+ n 1))"
    "(define (sub1 n) (- n 1))"
    "(define (abs x) (if (< x 0) (- x) x))"
    "(define (even? n) (= (modulo n 2) 0))"
    "(define (odd? n) (not (even? n)))"
    "(define (positive? x) (> x 0))"
    "(define (negative? x) (< x 0))"
    "(define (max x . xs) (fold-left (lambda (a b) (if (> b a) b a)) x xs))"
    "(define (min x . xs) (fold-left (lambda (a b) (if (< b a) b a)) x xs))"
    "(define (expt base e) (let loop ((e e) (acc 1)) (if (= e 0) acc (loop (- e 1) (* acc base)))))"
    "(define (gcd2 a b) (if (= b 0) a (gcd2 b (modulo a b))))"
    "(define (gcd . xs) (fold-left gcd2 0 (map abs xs)))"

    // --- list utilities ---
    "(define (list? x) (cond ((null? x) #t) ((pair? x) (list? (cdr x))) (else #f)))"
    "(define (list-tail lst k) (if (= k 0) lst (list-tail (cdr lst) (- k 1))))"
    "(define (fold-left f acc lst)"
    "  (if (null? lst) acc (fold-left f (f acc (car lst)) (cdr lst))))"
    "(define (fold-right f acc lst)"
    "  (fold-left (lambda (a x) (f x a)) acc (reverse lst)))"
    "(define (reduce f ridentity lst)"
    "  (if (null? lst) ridentity (fold-left f (car lst) (cdr lst))))"
    "(define (filter pred lst)"
    "  (reverse (fold-left (lambda (acc x) (if (pred x) (cons x acc) acc)) '() lst)))"
    "(define (assq key lst)"
    "  (cond ((null? lst) #f) ((eq? (caar lst) key) (car lst)) (else (assq key (cdr lst)))))"
    // NB: assv/memv are aliased to assq/memq -- correct for fixnum keys (eq? is
    // value-equal for unboxed fixnums) but uses eq? not eqv? for flonum keys.
    "(define assv assq)"
    "(define (assoc key lst)"
    "  (cond ((null? lst) #f) ((equal? (caar lst) key) (car lst)) (else (assoc key (cdr lst)))))"
    "(define (memq x lst)"
    "  (cond ((null? lst) #f) ((eq? x (car lst)) lst) (else (memq x (cdr lst)))))"
    "(define memv memq)"
    "(define (member x lst)"
    "  (cond ((null? lst) #f) ((equal? x (car lst)) lst) (else (member x (cdr lst)))))"
    "(define (list-copy lst) (fold-right cons '() lst))"

    // --- misc ---
    "(define (boolean=? a b) (eq? a b))"
    "(define (identity x) x)";

int lisp_load_prelude(lisp_value env) {
    const char *err = NULL;
    lisp_value r = lisp_eval_string(PRELUDE, env, &err);
    if (r == LISP_UNDEF && err != NULL)
        return -1;  // a bug in the prelude itself
    return 0;
}
