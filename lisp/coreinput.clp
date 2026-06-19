;; coreinput: the asynchronous input service, the canonical Cardinal Lisp server.
;;
;; The old C CoreInput (servers/CoreInput) was a device table plus a kernel poll
;; task that drained each device's has_pending/read into an event queue. Here it
;; is a long-lived restricted context owning a device list: input drivers `send`
;; it (register <name>) when they come up and (event <payload>) per input event.
;; There is no synchronous callback ABI, so the rx-handler-re-enters-tx
;; self-deadlock that the C servers had to tiptoe around cannot arise by
;; construction -- a `send` only enqueues.
;;
;; This is the mold every ported server follows: (import driver-util) for the
;; generic `serve` (spawn a restricted recv-loop threading some state), export a
;; single bring-up entry point, and dispatch messages by their head tag. The
;; service holds NO capabilities (serve grants '()): it only routes messages, so
;; it needs no hardware authority -- least privilege. The POLICY of which driver
;; feeds it lives in `init`, not here; this module is pure mechanism and is reused
;; unchanged by any input source (ps2 today, USB HID later).

(define-module coreinput
  (export start-input-service)
  (import driver-util)

  ;; Spawn the input service; returns the handle drivers `send` to. `devs` is the
  ;; registered-device list, threaded through the message loop by `serve`.
  (define (start-input-service)
    (serve '()
      (lambda (devs m)
        (cond ((eq? (car m) 'register)
               (display "[coreinput] device registered: ")
               (display (cadr m)) (newline)
               (cons (cadr m) devs))
              ((eq? (car m) 'event)
               (display "[coreinput] event ") (display (cadr m)) (newline)
               devs)
              (else devs))))))
