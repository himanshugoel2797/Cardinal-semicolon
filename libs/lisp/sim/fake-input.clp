;; fake-input: a host-backed input driver for the simulator.
;;
;; Stands in for the ps2 / usb-hid drivers. A real driver claims an IRQ and reads
;; a controller port; here the events come from the host window via the
;; host-input-poll prim. It feeds coreinput the SAME tuples the ps2 driver does
;;   (key <scancode> <pressed 1|0>)   -- raw PS/2 set-1 scancodes
;;   (pointer <x> <y> <down? #t|#f>)
;; so coreinput, the compositor, and any app decode them identically to a real
;; boot. The only difference from ps2.clp is the source of the bytes.
;;
;; A real driver blocks on (irq-wait); the host has no IRQ, and `sleep`/`uptime`
;; are kernel-only, so this pump cooperatively (yield)s between polls. The
;; simulator's run loop samples the host backend once per frame and queues the
;; events host-input-poll returns here.

(define-module fake-input
  (export start-fake-input)
  (import driver-util)

  ;; Spawn the pump feeding `coreinput`. Returns the pump context.
  (define (start-fake-input coreinput)
    (send coreinput (list 'register "Fake Host Input"))
    (spawn
      (lambda ()
        (let loop ()
          (for-each (lambda (ev) (send coreinput (list 'event ev)))
                    (host-input-poll))
          (yield)
          (loop))))))
