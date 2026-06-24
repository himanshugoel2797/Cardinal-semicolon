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
          corenetdebug coreusb ps2 virtio-net rtl8139 rtl8169 virtio-gpu lfb ahci
          cardfs hdaudio uhci xhci usb-hid usb-hub usb-storage sys-pci sys-cmdline)

  ;; Parse a dotted-quad "A.B.C.D" into (A B C D), or #f if malformed. Used for
  ;; the cardinal.ip= static-address override (digits/dots only, exactly 4 octets,
  ;; each 0..255). A bad value returns #f and falls back to DHCP rather than
  ;; silently configuring an out-of-range address that nothing would ever match.
  (define (parse-ipv4 s)
    (let loop ((chars (string->list s)) (cur 0) (have #f) (acc '()))
      (if (null? chars)
          (let ((acc2 (if have (cons cur acc) acc)))
            (if (= (length acc2) 4) (reverse acc2) #f))
          (let ((c (char->integer (car chars))))
            (cond
              ((= c 46)                          ; '.' -> next octet
               (if have (loop (cdr chars) 0 #f (cons cur acc)) #f))
              ((and (>= c 48) (<= c 57))         ; '0'..'9'
               (let ((n (+ (* cur 10) (- c 48))))
                 (if (> n 255) #f (loop (cdr chars) n #t acc))))
              (else #f))))))

  ;; The static address pinned on the kernel command line, or #f to use DHCP.
  (define (static-ip)
    (let ((v (cmdline-get "cardinal.ip=")))
      (if v (parse-ipv4 v) #f)))

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
    ;; `input` is bound here because the USB-HID class driver feeds the same
    ;; coreinput service the ps2 keyboard does; the whole bring-up runs in its scope.
    (let ((input (setup-input)))
    ;; Audio: start the service, capture its handle (formerly dropped), and bring
    ;; up the HD Audio controller feeding it. hdaudio-init is gated on pci-find, so
    ;; a default boot with no HDA controller just logs "no device" and returns.
    (let ((audio (start-audio-service)))
      (hdaudio-init audio))
    (start-power-service)
    ;; Storage registry (the AHCI block driver AND the USB mass-storage class
    ;; driver feed it); cardfs registers as an fs provider FIRST so it is offered
    ;; each block device that registers (a probe reads LBA 0 and claims the volume
    ;; if it is a cardfs superblock). The USB enumeration/dispatch service (coreusb)
    ;; comes up alongside. ahci-init/uhci-init/xhci-init are pci-find-gated, so a
    ;; default boot with no such device just logs "no device" and returns.
    (let ((storage (start-storage-service))
          (usb (start-usb-service)))
      (start-cardfs storage)
      (ahci-init storage)
      ;; Register the USB class drivers FIRST -- coreusb processes these
      ;; register-class messages (sent synchronously here) ahead of any later
      ;; port-connect, so the class table is populated when the first device
      ;; arrives. HID feeds coreinput; mass storage feeds the storage registry;
      ;; the hub recurses through coreusb.
      (usb-hid-init usb input)
      (usb-hub-init usb)
      (usb-storage-init usb storage)
      ;; Then bring up the host controllers.
      (uhci-init usb)
      (xhci-init usb))
    ;; Bring up the display registry, then the GPU driver, which brings the
    ;; virtio-gpu device to DRIVER_OK, paints a framebuffer, and registers itself
    ;; with the display service. Guarded: with no GPU present it just logs and
    ;; returns, so a headless smoke boot is unaffected.
    ;; Fallback policy: lfb claims the boot framebuffer (the firmware-provided
    ;; linear framebuffer, e.g. -vga std) ONLY when no virtio-gpu device is on the
    ;; bus. The pci-find gate is race-free -- it tests hardware presence, not a
    ;; registration result -- and matches the C "load lfb only if no display
    ;; registered". This gate would extend (an `or` over their pci-finds) if other
    ;; GPU drivers are added.
    (let ((display-svc (start-display-service)))
      (virtio-gpu-init display-svc)
      (if (not (pci-find #x1af4 #x1050)) (lfb-init display-svc)))
    ;; Bring up the network stack, then a NIC, which registers itself with the
    ;; stack and forwards frames to it. Prefer the proven virtio-net when present;
    ;; otherwise fall back to the rtl8139 (the `-device rtl8139` boot, where no
    ;; virtio-net exists). The register-nic the NIC sends sits ahead of the
    ;; messages below in the service's mailbox, so the MAC/TX are set first.
    ;;
    ;; Addressing policy (DHCP is the default; cardinal.ip=A.B.C.D forces a static
    ;; address and skips it): a static boot comes up at the pinned IP and primes
    ;; the ARP cache with a who-has for the slirp gateway -- the reply exercises
    ;; NIC RX -> service demux -> ARP cache end to end. A default boot comes up at
    ;; 0.0.0.0 and sends dhcp-start; the DHCP client acquires IP/mask/gw/dns and
    ;; learns the gateway MAC itself, so no manual prime is wanted there.
    (let* ((static (static-ip))
           (net (start-network-service (if static static (list 0 0 0 0)))))
      (cond ((pci-find #x1af4 #x1041) (virtio-net-init net))
            ((pci-find #x10ec #x8168) (rtl8169-init net))
            ((pci-find #x10ec #x8139) (rtl8139-init net))
            (else (display "[init] no supported NIC") (newline)))
      (if static
          (send net (list 'arp-request (list 10 0 2 2)))    ; static: prime gw who-has
          (send net (list 'dhcp-start)))                    ; default: configure via DHCP
      ;; Optional network debug endpoint (echo 1337 / digest 1338), gated on the
      ;; kernel command line -- a remote attack surface, so opt-in like the REPL.
      (if (cmdline-has? "cardinal.netdbg") (start-netdebug net)))
    'system-up))   ; close the (let ((input ...)) ...) that wraps the bring-up

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
