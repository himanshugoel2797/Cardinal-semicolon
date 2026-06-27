;; boot: the simulator's bring-up policy -- the host analogue of lisp/init.clp.
;;
;; It starts the REAL servers (coredisplay registry, coreinput fan-out) and the
;; FAKE host-backed drivers, then launches the demo app, wiring handles together
;; exactly as init.clp does on the target (which driver backs which service is
;; policy and lives here, not in the servers). Everything below this line is
;; ordinary message passing -- the host-specific part is confined to the fake
;; drivers' use of host-present! / host-input-poll / host-screen-size.

(define-module boot
  (export sim-start)
  (import coreinput coredisplay fake-display fake-input demo-app)

  (define (sim-start)
    (let ((sz (host-screen-size)))
      (let ((w (car sz)) (h (cadr sz)))
        (let ((display-reg (start-display-service))     ; real coredisplay registry
              (input-svc   (start-input-service)))       ; real coreinput fan-out
          (let ((disp (start-fake-display display-reg w h)))  ; fake display driver
            (start-fake-input input-svc)                 ; fake keyboard/mouse pump
            (run-demo-app input-svc disp)                ; the user app
            'sim-started))))))
