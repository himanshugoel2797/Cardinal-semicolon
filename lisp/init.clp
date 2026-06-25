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
  (export system-init start-repl play-tone set-vol)
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

  ;; The running coreaudio service handle, published by system-init so the serial
  ;; REPL can drive playback interactively (start-repl imports `play-tone` into the
  ;; REPL env). #f until the audio service is up.
  (define audio-service #f)

  ;; REPL command: play a tone through the audio stack. Routes through the live
  ;; coreaudio service to the registered card 'hda0 -- the exact path a real client
  ;; uses -- so it doubles as an end-to-end check of the hdaudio playback path.
  ;;   (play-tone)               default bring-up tone (500 Hz)
  ;;   (play-tone freq)          freq Hz at the default amplitude / length
  ;;   (play-tone freq amp)      + amplitude (1..32767)
  ;;   (play-tone freq amp n)    + length in frames (each frame = 1/48000 s)
  ;; Returns 'playing, or 'no-audio if no HD Audio controller came up.
  (define (play-tone . args)
    (cond
      ((not audio-service) 'no-audio)
      ((null? args)
       (send audio-service (list 'tone 'hda0)) 'playing)
      (else
       (let ((freq   (car args))
             (amp    (if (>= (length args) 2) (cadr args) 8000))
             (frames (if (>= (length args) 3) (caddr args) 4800)))
         ;; Guard arg types here: a non-integer would otherwise reach the card
         ;; context's (> freq 0) and kill it (the card handle then goes dead until
         ;; reboot). The card ALSO range-checks; this just stops bad types early.
         (if (and (integer? freq) (integer? amp) (integer? frames))
             (begin (send audio-service (list 'play 'hda0 freq amp frames)) 'playing)
             'bad-args)))))

  ;; REPL command: set an endpoint's volume on card 'hda0, through coreaudio.
  ;;   (set-vol ep-id vol)   vol 0..100 (0 mutes); ep-id from the boot endpoint log
  ;; Returns 'ok, 'no-audio, or 'bad-args.
  (define (set-vol ep-id vol)
    (cond
      ((not audio-service) 'no-audio)
      ((and (integer? ep-id) (integer? vol))
       (send audio-service (list 'set-volume 'hda0 ep-id vol)) 'ok)
      (else 'bad-args)))

  ;; Capture self-check (gated on cardinal.mictest): in its own context, find card
  ;; hda0's first input endpoint, start capturing, sample the DMA position twice,
  ;; and log whether it advanced (the capture engine is live) plus the captured
  ;; byte count. QEMU has no mic source so the PCM is silence, but the input stream
  ;; DMA still runs -- LPIB advancing is the proof the path works end to end.
  (define (start-mictest)
    (spawn-restricted '()
      (lambda ()
        (sleep 2000000000)                                  ; let hda0 finish bring-up
        (send audio-service (list 'endpoints 'hda0 (self)))
        (let find ((es (recv)))
          (cond
            ((null? es) (display "[mictest] no input endpoint") (newline))
            ((eq? (cadr (car es)) 'in)
             (let ((id (car (car es))))
               (send audio-service (list 'capture-start 'hda0 id))
               (sleep 300000000)
               (send audio-service (list 'capture-pos 'hda0 (self)))
               (let ((p1 (recv)))
                 (sleep 100000000)
                 (send audio-service (list 'capture-pos 'hda0 (self)))
                 (let ((p2 (recv)))
                   (send audio-service (list 'capture-read 'hda0 (self)))
                   (let ((buf (recv)))
                     (send audio-service (list 'capture-stop 'hda0))
                     (display "[mictest] ep ") (display id)
                     (display " pos ") (display p1) (display " -> ") (display p2)
                     (display (if (and p1 p2 (not (= p1 p2))) " ADVANCING" " (no advance)"))
                     (display " captured-bytes=") (display (if buf (bytes-length buf) 0))
                     (newline))))))
            (else (find (cdr es))))))))

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
      (set! audio-service audio)      ; publish for the REPL `play-tone` command
      ;; Bring up EVERY HD Audio controller, not just the first: pci-find-class-all
      ;; enumerates all class-0x04/0x03 functions, so a machine with two sound cards
      ;; registers both (hda0, hda1, ...). Each gets its own driver context + codec
      ;; enumeration. A default boot with no HDA controller just brings up none.
      (let ((idx 0))
        (for-each
         (lambda (ecam)
           (hdaudio-init audio (string->symbol (string-append "hda" (number->string idx))) ecam)
           (set! idx (+ idx 1)))
         (pci-find-class-all #x04 #x03)))
      ;; Optional capture self-check: start capturing on hda0's first input
      ;; endpoint and confirm the DMA engine runs (see start-mictest).
      (if (cmdline-has? "cardinal.mictest") (start-mictest))
      ;; Optional hotplug exercise: force a codec re-scan, driving the SAME
      ;; reconciliation the state-change interrupt would (scan-all-codecs ->
      ;; re-enumerate -> update the endpoint set). QEMU's hda bus can't actually
      ;; hot-add a codec, so this injects the rescan to validate the path end to end.
      (if (cmdline-has? "cardinal.hotplug")
          (spawn-restricted '()
            (lambda () (sleep 2500000000) (send audio-service (list 'rescan 'hda0)))))
      ;; Optional jack-detect exercise: force a pin-sense re-poll, then report each
      ;; endpoint's presence -- proves the poll read-path runs on real hardware
      ;; (QEMU's sense is static, so nothing changes, but the read must succeed).
      (if (cmdline-has? "cardinal.jacktest")
          (spawn-restricted '()
            (lambda ()
              (sleep 2500000000)
              ;; both messages hit the driver's mailbox in order (single-context
              ;; FIFO at each hop), so the poll is processed before the endpoints
              ;; query -- no sleep needed between them.
              (send audio-service (list 'poll-jacks 'hda0))
              (send audio-service (list 'endpoints 'hda0 (self)))
              (for-each (lambda (d)
                          (display "[jacktest] ep ") (display (car d))
                          (display " ") (display (nth d 2))
                          (display (if (nth d 3) " present" " absent")) (newline))
                        (recv))))))
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
           (net (start-network-service)))
      ;; Bring up EVERY supported NIC, not just the first: pci-find-all enumerates
      ;; all matching devices (so two virtio-nets, or a virtio-net + an rtl, both
      ;; come up), and each driver-init registers its device as its own interface.
      ;; `bring-up` runs a driver over each ecam of a (vid did) and counts NICs.
      (let ((nics 0))
        (define (bring-up vid did drv)
          (for-each (lambda (ecam) (drv net ecam) (set! nics (+ nics 1)))
                    (pci-find-all vid did)))
        (bring-up #x1af4 #x1041 virtio-net-init)
        (bring-up #x10ec #x8168 rtl8169-init)
        (bring-up #x10ec #x8139 rtl8139-init)
        (if (= nics 0) (begin (display "[init] no supported NIC") (newline))))
      ;; The register-nic each NIC sends sits ahead of these in the service mailbox,
      ;; so every interface exists before we address it. Static: assign the pinned
      ;; IP on a /24 to the primary interface (mac #f) and prime the gw who-has.
      ;; Default: DHCP on EVERY interface (dhcp-start-all); each client configures
      ;; its own interface (the OFFER/ACK fan out on port 68, filtered by xid/MAC).
      (if static
          (begin
            (send net (list 'set-address #f static (list 255 255 255 0) (list 0 0 0 0) (list 0 0 0 0)))
            (send net (list 'arp-request (list 10 0 2 2))))
          (send net (list 'dhcp-start-all)))                ; default: configure via DHCP
      ;; Optional network debug endpoint (echo 1337 / digest 1338), gated on the
      ;; kernel command line -- a remote attack surface, so opt-in like the REPL.
      (if (cmdline-has? "cardinal.netdbg") (start-netdebug net))
      ;; Test-only TCP fault injection: cardinal.tcploss makes the stack drop 1 in
      ;; 4 received segments, exercising retransmission + out-of-order reassembly.
      (if (cmdline-has? "cardinal.tcploss") (send net (list 'tcp-test-loss 4)))
      ;; Test-only firewall demo: cardinal.fwtest installs a rule that DENIES
      ;; inbound TCP to port 7, so the netdbg TCP echo becomes unreachable while
      ;; everything else (DHCP/DNS/UDP echo) still works -- a visible deny.
      (if (cmdline-has? "cardinal.fwtest")
          (send net (list 'fw-add 'deny 6 'any 0 7)))
      ;; Test-only DNS probe: resolve a hostname via the DHCP-learned server and
      ;; print the result (after DHCP has had time to bind the interface).
      (if (cmdline-has? "cardinal.dnstest")
          (spawn-restricted '()
            (lambda ()
              (sleep 3000000000)
              (display "[dnstest] example.com -> ")
              (display (dns-resolve net "example.com")) (newline)))))
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
              ;; Expose init's REPL command(s) in the REPL's persistent env so they
              ;; can be called bare, e.g. (play-tone 440). repl-eval evaluates in
              ;; that env; init is already loaded, so this just binds the export
              ;; (idempotent), it does not re-run system-init. The (only play-tone)
              ;; clause is deliberate: importing all of init would also expose
              ;; system-init / start-repl, which a stray REPL call could use to
              ;; re-run the whole boot or spawn a second REPL that steals COM1.
              (repl-eval "(import (init (only play-tone set-vol)))")
              (display "[repl] serial REPL ready on COM1 -- try (play-tone)") (newline)
              (console-flush)                        ; log is batched -- push the banner out now
              (let loop ((seen (irq-count irq)))
                (let ((in (console-poll)))
                  (if in
                      (begin (console-write (repl-eval in)) (console-flush) (loop (irq-count irq)))
                      (begin (irq-wait irq seen) (loop (irq-count irq)))))))))))) ) ; last ) closes (define-module init ...)
