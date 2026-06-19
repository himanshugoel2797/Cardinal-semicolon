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
  (export system-init start-repl)
  (import coreinput coreaudio corepower corestorage coredisplay corenetwork
          ps2 virtio-net virtio-gpu)

  ;; Bring up keyboard input: start the generic input service (coreinput, a
  ;; reusable server module -- mechanism), run i8042 bring-up here in the root
  ;; init context (ps2-init wants port-I/O authority, which init has and the
  ;; restricted pump does not need to repeat), then spawn the keyboard pump as a
  ;; restricted context that feeds the service. This wiring -- WHICH driver backs
  ;; the input service -- is policy, so it lives in init, not in coreinput.
  (define (setup-input)
    (let ((input (start-input-service)))   ; the async device-table server
      (ps2-init)                           ; i8042 controller + keyboard bring-up
      (spawn-restricted '()                ; the keyboard pump needs no import authority
        (lambda () (ps2-keyboard-driver input)))
      input))

  ;; The system entry point: called once on the BSP after the scheduler is live.
  ;; Each Core* service is a long-lived context; bring up the ones that exist
  ;; today (input, audio, power) and the NIC. Audio/power have no drivers feeding
  ;; them yet -- they idle parked on recv -- but the endpoints are present for the
  ;; drivers that will attach (the same posture the C servers held).
  (define (system-init)
    (setup-input)
    (start-audio-service)
    (start-power-service)
    (start-storage-service)
    ;; Bring up the display registry, then the GPU driver, which brings the
    ;; virtio-gpu device to DRIVER_OK, paints a framebuffer, and registers itself
    ;; with the display service. Guarded: with no GPU present it just logs and
    ;; returns, so a headless smoke boot is unaffected.
    (let ((display-svc (start-display-service)))
      (virtio-gpu-init display-svc))
    ;; Bring up the network stack, then the NIC, which registers itself with the
    ;; stack and forwards frames to it. Prime the ARP cache with a who-has for the
    ;; slirp gateway -- the reply exercises NIC RX -> service demux -> ARP cache
    ;; end to end (the live counterpart to the in-OS network self-test). The
    ;; register-nic the NIC sends sits ahead of this arp-request in the service's
    ;; mailbox, so the MAC/TX are set before the request is built.
    (let ((net (start-network-service (list 10 0 2 15))))   ; slirp guest address
      (virtio-net-init net)
      (send net (list 'arp-request (list 10 0 2 2))))       ; who-has the gateway
    'system-up)

  ;; The interactive serial REPL (started only under cardinal.repl). A root context
  ;; -- a serial shell for debugging the OS is exactly the case that wants full
  ;; authority -- it imports the console I/O + the ISA-IRQ bridge, claims COM1 RX
  ;; (IRQ 4), arms the UART receive interrupt, then PARKS on the line: an arriving
  ;; byte wakes it, it drains + evaluates the input and writes the transcript back,
  ;; then parks again. No busy-polling -- the BSP idles between keystrokes. `seen`
  ;; is captured before each drain so a byte landing mid-drain is never lost (the
  ;; count-based wake, same as the ps2 pump).
  (define (start-repl)
    (spawn (lambda ()
      (import sys-console sys-irq)
      (let ((irq (irq-register 4)))               ; COM1 receive = ISA IRQ 4
        (if (not irq)
            (begin (display "[repl] irq-register failed") (newline))
            (begin
              (console-arm-rx)                     ; enable COM1 RX IRQ now it is routed
              (display "[repl] serial REPL ready on COM1") (newline)
              (console-flush)                        ; log is batched -- push the banner out now
              (let loop ((seen (irq-count irq)))
                (let ((in (console-poll)))
                  (if in
                      (begin (console-write (repl-eval in)) (console-flush) (loop (irq-count irq)))
                      (begin (irq-wait irq seen) (loop (irq-count irq)))))))))))) ) ; last ) closes (define-module init ...)
