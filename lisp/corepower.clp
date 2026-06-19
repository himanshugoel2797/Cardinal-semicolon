;; corepower: the power-management service, ported from servers/CorePower.
;;
;; The C server kept a queue of pwr_device_t records (a name, a device-class
;; bitmask, and two callbacks event_g/event_d) and, on a state-change event,
;; called every registered device whose class intersected the event's class. The
;; synchronous callbacks are the part that does not survive the move to Lisp: here
;; a power device is just another context, and the service fans an event out by
;; `send`ing it -- no re-entrant callback, so a device that reacts to a power
;; event (and might talk to the service again) can never deadlock it.
;;
;; Protocol (everything is a `send` to the service handle):
;;   (register <name> <class-bits> <ctx>)  -- a device joins; ctx is its context
;;   (event-g  <class-bits> <gstate> <pstate>) -- global power transition
;;   (event-d  <class-bits> <dstate>)          -- device power transition
;; Each matching device then receives (pwr-g <gstate> <pstate>) / (pwr-d <dstate>)
;; -- the message form of the old event_g/event_d entry points.

(define-module corepower
  (export start-power-service
          pwr-generic pwr-display pwr-audio-out pwr-audio-in
          pwr-hid pwr-camera pwr-processor)
  (import driver-util)

  ;; device_pwr_class bits (mirror servers/inc/CorePower/power.h exactly).
  (define pwr-generic   1)
  (define pwr-display   2)
  (define pwr-audio-out 4)
  (define pwr-audio-in  8)
  (define pwr-hid      16)
  (define pwr-camera   32)
  (define pwr-processor 64)

  ;; Deliver `msg` to every registered device whose class bitmask intersects the
  ;; event's class. A registered entry is (name class ctx).
  (define (pwr-fanout devs class msg)
    (for-each (lambda (d)
                (if (not (= 0 (bitwise-and (cadr d) class)))
                    (send (caddr d) msg)))
              devs))

  ;; Spawn the power service; returns the handle to `send` events/registrations
  ;; to. `devs` is the registered-device list ((name class ctx) ...).
  (define (start-power-service)
    (serve '()
      (lambda (devs m)
        (cond ((eq? (car m) 'register)         ; (register name class ctx)
               (display "[corepower] device registered: ")
               (display (cadr m)) (newline)
               (cons (cdr m) devs))            ; store (name class ctx)
              ((eq? (car m) 'event-g)          ; (event-g class gstate pstate)
               (pwr-fanout devs (cadr m) (list 'pwr-g (caddr m) (cadddr m)))
               devs)
              ((eq? (car m) 'event-d)          ; (event-d class dstate)
               (pwr-fanout devs (cadr m) (list 'pwr-d (caddr m)))
               devs)
              (else devs))))))
