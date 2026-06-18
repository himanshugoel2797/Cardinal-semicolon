;; Copyright (c) 2026 Himanshu Goel
;; This software is released under the MIT License.
;; https://opensource.org/licenses/MIT
;;
;; Curated R7RS-small conformance subset for the Cardinal; kernel Scheme. Uses a
;; thunk-based harness (no macros needed yet). Cases are restricted to features
;; the interpreter currently supports; the runner (test_conformance.c) checks the
;; global `fail` count. Grow this as features land; the rational/bignum tower and
;; macros/call-cc are deferred. See notes/core/lisp-substrate.md.

(define pass 0)
(define fail 0)

(define (report name expected got)
  (set! fail (+ fail 1))
  (display "  FAIL ") (display name)
  (display ": expected ") (write expected)
  (display " got ") (write got) (newline))

(define (test name thunk expected)
  (let ((got (thunk)))
    (if (equal? got expected)
        (set! pass (+ pass 1))
        (report name expected got))))

;; --- Booleans / equivalence ---
(test "not-true"    (lambda () (not #f)) #t)
(test "not-zero"    (lambda () (not 0)) #f)         ; 0 is truthy in Scheme
(test "eq-sym"      (lambda () (eq? 'a 'a)) #t)
(test "eqv-num"     (lambda () (eq? 5 5)) #t)
(test "equal-list"  (lambda () (equal? '(1 (2) 3) '(1 (2) 3))) #t)
(test "equal-str"   (lambda () (equal? "ab" "ab")) #t)
(test "equal-vec"   (lambda () (equal? #(1 2) #(1 2))) #t)

;; --- Pairs and lists ---
(test "car"         (lambda () (car '(a b c))) 'a)
(test "cdr"         (lambda () (cdr '(a b c))) '(b c))
(test "cons"        (lambda () (cons 1 2)) '(1 . 2))
(test "list"        (lambda () (list 1 2 3)) '(1 2 3))
(test "length"      (lambda () (length '(a b c d))) 4)
(test "append"      (lambda () (append '(1 2) '(3) '(4 5))) '(1 2 3 4 5))
(test "reverse"     (lambda () (reverse '(1 2 3))) '(3 2 1))
(test "list-ref"    (lambda () (list-ref '(a b c) 1)) 'b)
(test "null?"       (lambda () (null? '())) #t)
(test "pair?"       (lambda () (pair? '(1))) #t)

;; --- Numbers: integers ---
(test "add"         (lambda () (+ 1 2 3 4)) 10)
(test "sub"         (lambda () (- 10 3 2)) 5)
(test "mul"         (lambda () (* 2 3 4)) 24)
(test "neg"         (lambda () (- 5)) -5)
(test "modulo"      (lambda () (modulo 17 5)) 2)
(test "div-exact"   (lambda () (/ 6 2)) 3)
(test "lt-chain"    (lambda () (< 1 2 3)) #t)
(test "cmp-eq"      (lambda () (= 7 7)) #t)
(test "zero?"       (lambda () (zero? 0)) #t)

;; --- Numbers: flonums ---
(test "fadd"        (lambda () (+ 1.5 2.5)) 4.0)
(test "contagion"   (lambda () (+ 1 2.0)) 3.0)
(test "div-inexact" (lambda () (/ 7 2)) 3.5)
(test "mixed-cmp"   (lambda () (= 2 2.0)) #t)
(test "integer-fl"  (lambda () (integer? 2.0)) #t)
(test "exact->in"   (lambda () (exact->inexact 4)) 4.0)
(test "in->exact"   (lambda () (inexact->exact 3.0)) 3)

;; --- Symbols / chars / strings ---
(test "symbol?"     (lambda () (symbol? 'x)) #t)
(test "char->int"   (lambda () (char->integer #\A)) 65)
(test "int->char"   (lambda () (integer->char 97)) #\a)
(test "char-upcase" (lambda () (char-upcase #\a)) #\A)
(test "str-len"     (lambda () (string-length "hello")) 5)
(test "str-ref"     (lambda () (string-ref "abc" 1)) #\b)
(test "substring"   (lambda () (substring "hello world" 0 5)) "hello")
(test "str-append"  (lambda () (string-append "foo" "bar")) "foobar")
(test "str->list"   (lambda () (string->list "ab")) '(#\a #\b))
(test "list->str"   (lambda () (list->string '(#\h #\i))) "hi")
(test "sym->str"    (lambda () (symbol->string 'abc)) "abc")
(test "str->num"    (lambda () (string->number "42")) 42)
(test "str->num-f"  (lambda () (string->number "xyz")) #f)
(test "num->str"    (lambda () (number->string 42)) "42")
(test "str=?"       (lambda () (string=? "ab" "ab")) #t)

;; --- Vectors ---
(test "vector"      (lambda () (vector 1 2 3)) #(1 2 3))
(test "vec-ref"     (lambda () (vector-ref #(10 20 30) 1)) 20)
(test "vec-len"     (lambda () (vector-length #(1 2 3 4))) 4)
(test "vec->list"   (lambda () (vector->list #(1 2 3))) '(1 2 3))
(test "list->vec"   (lambda () (list->vector '(a b c))) #(a b c))

;; --- Control ---
(test "if-then"     (lambda () (if (< 1 2) 'yes 'no)) 'yes)
(test "cond"        (lambda () (cond (#f 1) (#t 2) (else 3))) 2)
(test "cond-else"   (lambda () (cond (#f 1) (else 'e))) 'e)
(test "and"         (lambda () (and 1 2 3)) 3)
(test "and-false"   (lambda () (and 1 #f 3)) #f)
(test "or"          (lambda () (or #f #f 7)) 7)
(test "begin"       (lambda () (begin 1 2 3)) 3)

;; --- Binding forms ---
(test "let"         (lambda () (let ((a 1) (b 2)) (+ a b))) 3)
(test "let*"        (lambda () (let* ((a 1) (b (+ a 1))) (+ a b))) 3)
(test "letrec"
  (lambda ()
    (letrec ((ev? (lambda (n) (if (= n 0) #t (od? (- n 1)))))
             (od? (lambda (n) (if (= n 0) #f (ev? (- n 1))))))
      (ev? 10)))
  #t)
(test "named-let"
  (lambda () (let loop ((i 0) (acc 0)) (if (= i 5) acc (loop (+ i 1) (+ acc i)))))
  10)

;; --- Closures / recursion / higher-order ---
(test "closure"
  (lambda ()
    (define (adder n) (lambda (x) (+ x n)))
    ((adder 5) 100))
  105)
(test "factorial"
  (lambda ()
    (define (fact n) (if (= n 0) 1 (* n (fact (- n 1)))))
    (fact 6))
  720)
(test "map"         (lambda () (map (lambda (x) (* x x)) '(1 2 3 4))) '(1 4 9 16))
(test "apply"       (lambda () (apply + '(1 2 3 4))) 10)

;; --- Quasiquote ---
(test "qq-simple"   (lambda () (let ((x 5)) `(a ,x c))) '(a 5 c))
(test "qq-splice"   (lambda () (let ((xs '(1 2 3))) `(0 ,@xs 4))) '(0 1 2 3 4))
(test "qq-nested"   (lambda () `(a (b ,(+ 1 2)))) '(a (b 3)))

;; --- Summary (read by the runner via the global `fail`) ---
(display "conformance subset: ")
(display pass) (display " passed, ")
(display fail) (display " failed")
(newline)
