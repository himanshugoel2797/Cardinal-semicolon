;; ps2: the i8042 PS/2 keyboard controller, written in Cardinal Lisp.
;;
;; This replaces the former C driver (drivers/ps2) entirely. Loaded from the
;; initrd at boot (single-core, into the shared env) as pure definitions; SysLisp
;; calls (ps2-init) and spawns (ps2-keyboard-driver coreinput) from the input
;; service. The whole driver -- controller bring-up, keyboard init, and the
;; IRQ-driven scancode pump -- is Lisp over the driver substrate: the legacy
;; port-I/O prims (in-u8/out-u8), the bitwise prims, and the generic ISA-IRQ wake
;; bridge (irq-register/irq-count/irq-wait).
;;
;; The one piece that cannot be Lisp is the interrupt-context trampoline that
;; wakes the parked driver context -- alloc and GC are illegal in interrupt
;; context -- and that lives in SysLisp as lisp_irq_isr, shared by every ISA
;; driver. So this is zero ps2-specific C: the native floor is generic.
;;
;; Decode is deferred OUT of interrupt context: lisp_irq_isr only wakes us; this
;; context then drains the i8042 output buffer with in-u8. The i8042 has a single
;; one-byte output buffer whose Output-Buffer-Full bit gates the next IRQ, so a
;; byte is never lost while we are scheduled to drain it -- only throughput drops
;; under load. KEYBOARD ONLY for now (the proven, consumed path): the mouse is a
;; later refinement, so port 2 is left disabled and never raises an IRQ.

;; --- i8042 ports + status bits ----------------------------------------------

;; The driver is a module: it imports exactly the capabilities it needs -- sys-io
;; (legacy port I/O for the i8042) and sys-irq (the generic ISA-IRQ wake bridge)
;; -- and exports only its two entry points. The port-I/O and IRQ primitives are
;; private to this module; code outside cannot reach them through `ps2`. (Bodies
;; stay at column 0: this is a pure wrapper over the existing definitions.)
(define-module ps2
  (export ps2-init ps2-keyboard-driver)
  (import sys-io sys-irq)

(define PS2-DATA #x60)
(define PS2-CMD  #x64)          ; write: command; read: status

(define PS2-STATUS-OBF  1)      ; bit0: output buffer full (a byte is readable)
(define PS2-STATUS-IBF  2)      ; bit1: input buffer full  (controller still busy)
(define PS2-STATUS-MOUSE #x20)  ; bit5: the readable byte came from port 2 (mouse)

;; A bounded spin keeps a wedged/absent controller (which can read back 0xFF on
;; every port, i.e. IBF stuck set) from hanging boot -- strictly safer than the
;; old C, which spun unbounded. The cap is generous: real i8042s answer in a few
;; reads, and this only runs at boot.
(define PS2-SPIN 1000000)

(define (ps2-status) (in-u8 PS2-CMD))

;; Spin until the input buffer is empty (safe to write), or the cap elapses.
(define (ps2-wait-write)
  (let loop ((n PS2-SPIN))
    (if (= n 0) #f
        (if (= 0 (bitwise-and (ps2-status) PS2-STATUS-IBF)) #t
            (loop (- n 1))))))

;; Spin until a byte is available (OBF set), or the cap elapses.
(define (ps2-wait-read)
  (let loop ((n PS2-SPIN))
    (if (= n 0) #f
        (if (= 0 (bitwise-and (ps2-status) PS2-STATUS-OBF)) (loop (- n 1))
            #t))))

(define (ps2-cmd b)   (ps2-wait-write) (out-u8 PS2-CMD b))
(define (ps2-write b)  (ps2-wait-write) (out-u8 PS2-DATA b))
;; Read a byte, waiting (bounded) for one; returns the byte, or -1 on timeout.
(define (ps2-read)
  (if (ps2-wait-read) (in-u8 PS2-DATA) -1))

;; Drain any pending output so a stale byte doesn't desync init.
(define (ps2-flush)
  (let loop ((n PS2-SPIN))
    (if (= n 0) #f
        (if (= 0 (bitwise-and (ps2-status) PS2-STATUS-OBF)) #t
            (begin (in-u8 PS2-DATA) (loop (- n 1)))))))

;; --- keyboard device init ----------------------------------------------------

;; Enable scanning on the keyboard. We deliberately do NOT reset (0xFF) or force a
;; scancode set: a reset triggers a long BAT that can overrun the bounded waits and
;; desync the ACK stream, and the power-on default (scanning enabled, the firmware's
;; set/translation) already works. The pump forwards raw codes set-agnostically, so
;; the active set does not matter; 0xF0 (the set-2/3 break prefix) is decoded as a
;; release. ps2-read consumes the 0xFA ACK.
(define (ps2-kbd-init)
  (ps2-write #xF4) (ps2-read)              ; enable scanning, consume ACK
  (ps2-flush))

;; Bring up the i8042 controller and the keyboard. Returns #t on success, #f if the
;; controller self-test fails (no PS/2 hardware) -- matching the old C, which probed
;; regardless of the FADT 8042 flag and bailed cleanly when absent. Translation
;; (config bit 6) is left AS THE FIRMWARE SET IT: the pump is set-agnostic, and
;; forcing translation off changes what some i8042s emit.
(define (ps2-init)
  (ps2-cmd #xAD)        ; disable port 1
  (ps2-cmd #xA7)        ; disable port 2 (kept disabled: keyboard-only for now)
  (ps2-flush)
  (ps2-cmd #x20)        ; read controller config byte
  (let ((cfg (bitwise-and (ps2-read)
                          (bitwise-not (bitwise-or 1 2)))))  ; quiet both port IRQs during setup
    (ps2-cmd #x60) (ps2-write cfg)         ; write config back
    (ps2-cmd #xAA)                          ; controller self-test
    (if (not (= (ps2-read) #x55))
        (begin (display "[ps2] controller self-test failed; no PS/2 input") (newline) #f)
        (begin
          (ps2-cmd #x60) (ps2-write cfg)    ; re-write (self-test can reset it)
          (ps2-cmd #xAE)                    ; enable port 1
          (ps2-kbd-init)
          (ps2-cmd #x20)                    ; re-read config, then set the keyboard IRQ bit (0)
          (let ((cfg2 (bitwise-or (ps2-read) 1)))
            (ps2-cmd #x60) (ps2-write cfg2))
          (display "[ps2] keyboard up") (newline)
          #t))))

;; --- the IRQ-driven scancode pump -------------------------------------------

;; Drain every byte currently in the output buffer, forwarding keyboard events to
;; `coreinput` as (event (key <scancode> <pressed?>)). A byte whose status shows
;; the mouse bit is read and discarded (port 2 is disabled, so this is just
;; defensive). 0xFA is the command ACK and is ignored; 0xF0 is the set-2 break
;; prefix, so the following byte is a release.
(define (ps2-drain coreinput)
  (let loop ()
    (if (= 0 (bitwise-and (ps2-status) PS2-STATUS-OBF))
        #f                                  ; buffer empty -> done
        (begin
          (if (= 0 (bitwise-and (ps2-status) PS2-STATUS-MOUSE))
              (let ((c (in-u8 PS2-DATA)))   ; keyboard byte
                (cond ((= c #xFA) #f)       ; ACK: ignore
                      ((= c #xF0)           ; break prefix: next byte is the released key
                       (send coreinput (list 'event (list 'key (ps2-read) 0))))
                      (else
                       (send coreinput (list 'event (list 'key c 1))))))
              (in-u8 PS2-DATA))             ; mouse byte: discard
          (loop)))))

;; Prove the IRQ pipeline at boot, without external input: send the keyboard an
;; echo (0xEE); its reply lands in the output buffer and -- the keyboard IRQ now
;; being enabled -- fires the real IRQ 1, exercising ISR -> wake -> drain end to
;; end. The reply is 0xEE (not a scancode), so consume it here rather than forward
;; it. Logs "[ps2] irq self-test ok" so a headless smoke test can assert the
;; interrupt path (QEMU does not deliver injected keystrokes to the PS/2 device).
(define (ps2-irq-selftest irq)
  (let ((seen (irq-count irq)))
    (ps2-write #xEE)                        ; proper write: waits for IBF clear first
    (irq-wait irq seen)
    ;; Scan the drained bytes for the echo, skipping (consuming) any byte that
    ;; raced the just-unmasked line ahead of the reply -- so a stale byte neither
    ;; masquerades as the echo nor leaks into the pump as a bogus key event.
    (let scan ((tries 4))
      (let ((b (ps2-read)))
        (cond ((= b #xEE)
               (display "[ps2] irq self-test ok (echo via IRQ 1)") (newline))
              ((= tries 0)
               (display "[ps2] irq self-test: no echo reply") (newline))
              (else (scan (- tries 1))))))
    (ps2-flush)))

;; The keyboard driver context. Claims IRQ 1, registers with coreinput, self-tests
;; the interrupt path, then loops: drain, then park until the next keyboard IRQ.
;; `seen` is captured BEFORE draining so a key landing during the drain advances
;; the counter and makes irq-wait return immediately (re-drain) instead of parking
;; on a byte already in the buffer -- the count-based, never-missed wake.
(define (ps2-keyboard-driver coreinput)
  (send coreinput (list 'register 'ps2-keyboard))
  (let ((irq (irq-register 1)))
    (if (not irq)
        (begin (display "[ps2] irq-register failed") (newline) #f)
        (begin
          (ps2-irq-selftest irq)
          (let pump ((seen (irq-count irq)))
            (ps2-drain coreinput)
            (irq-wait irq seen)
            (pump (irq-count irq)))))))) ; last ) closes (define-module ps2 ...)
