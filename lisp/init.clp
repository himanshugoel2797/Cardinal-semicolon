;; init: the privileged system initializer -- the first Lisp the OS runs, and the
;; single place boot POLICY lives. The kernel only loads this module and calls
;; (system-init); everything about WHAT comes up and with WHICH capabilities is
;; here, not hardcoded in C. (Replaces the former setup_input_service/discover_nic
;; in modules/SysLisp.)
;;
;; Capability posture (W7). `init` is loaded at boot with full (root) authority,
;; so its (import ps2 virtio-net) succeeds: each driver module captures exactly
;; the sys-* authority it needs into its own closures at load time. The long-lived
;; service contexts init then spawns are RESTRICTED -- (spawn-restricted '() ...)
;; grants them NO further import authority -- so a wedged or compromised driver
;; loop cannot (import sys-pci) to reach new hardware. They still work because the
;; module boundary already handed them their primitives lexically; the empty grant
;; only forbids acquiring MORE. This is the knob a future serial REPL rides: root
;; when debugging the OS, a narrow grant when not.

(define-module init
  (export system-init)
  (import ps2 virtio-net)

  ;; The async input service: a long-lived context owning a device table and
  ;; routing input events. Drivers `send` it (register <name>) and (event
  ;; <payload>); there is no synchronous re-entry, so the network-style "rx
  ;; handler calls back into tx" self-deadlock cannot arise here by construction.
  ;; Returns the coreinput handle the keyboard driver sends to.
  (define (start-input-service)
    (let ((coreinput
            (spawn-restricted '()
              (lambda ()
                (let loop ((devs '()))
                  (let ((m (recv)))
                    (cond ((eq? (car m) 'register)
                           (display "[coreinput] device registered: ")
                           (display (cadr m)) (newline)
                           (loop (cons (cadr m) devs)))
                          ((eq? (car m) 'event)
                           (display "[coreinput] event ") (display (cadr m)) (newline)
                           (loop devs))
                          (else (loop devs)))))))))
      (ps2-init)                  ; i8042 bring-up runs here, in the root init context
      (spawn-restricted '()       ; the keyboard pump needs no import authority
        (lambda () (ps2-keyboard-driver coreinput)))
      coreinput))

  ;; The system entry point: called once on the BSP after the scheduler is live.
  (define (system-init)
    (start-input-service)
    (virtio-net-init)             ; brings up the NIC (no-op if absent); spawns its own RX context
    'system-up))
