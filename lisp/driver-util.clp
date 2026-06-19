;; driver-util: generic helpers shared by Lisp device drivers.
;;
;; The first Cardinal Lisp *library* -- a module (define-module) rather than a
;; flat top-level program. Drivers pull these in with (import driver-util)
;; instead of redefining them per file. Everything here is device-agnostic:
;; list indexing and a mutable word cell built on the byte-buffer primitive
;; (the language's pairs/vectors are immutable, so a 1-element bytes buffer is
;; the mutable store the driver substrate needs).

(define-module driver-util
  (export nth make-cell cell-ref cell-set!)

  (define (nth lst k) (if (= k 0) (car lst) (nth (cdr lst) (- k 1))))

  (define (make-cell v) (let ((b (make-bytes 8))) (bytes-u64-set! b 0 v) b))
  (define (cell-ref c)  (bytes-u64-ref c 0))
  (define (cell-set! c v) (bytes-u64-set! c 0 v)))
