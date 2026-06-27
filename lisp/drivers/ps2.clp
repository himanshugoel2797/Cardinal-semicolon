;; ps2: the i8042 PS/2 keyboard controller, written in Cardinal Lisp.
;;
;; This replaces the former C driver (drivers/ps2) entirely. Imported at boot by
;; the `init` module (single-core); init's input service calls (ps2-init) and
;; spawns (ps2-keyboard-driver coreinput) -- the latter as a restricted context
;; (no import authority). The whole driver -- controller bring-up, keyboard init,
;; and the IRQ-driven scancode pump -- is Lisp over the driver substrate: the legacy
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
;; under load. Both ports are brought up: port 1 (keyboard, IRQ1) and port 2
;; (mouse/aux, IRQ12). The single pump drains the shared 0x60 output buffer and
;; routes each byte by the status mouse bit (0x20) -- keyboard bytes to the key
;; path, mouse bytes through a 3-byte packet framer that emits pointer events.

;; --- i8042 ports + status bits ----------------------------------------------

;; The driver is a module: it imports exactly the capabilities it needs -- sys-io
;; (legacy port I/O for the i8042) and sys-irq (the generic ISA-IRQ wake bridge)
;; -- and exports only its two entry points. The port-I/O and IRQ primitives are
;; private to this module; code outside cannot reach them through `ps2`. (Bodies
;; stay at column 0: this is a pure wrapper over the existing definitions.)
(define-module ps2
  (export ps2-init ps2-keyboard-driver
          ;; pure, host-testable decode/accumulate (no hardware): exported so the
          ;; unit test can exercise the packet math without an i8042.
          ps2-mouse-decode ps2-mouse-accum)
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

;; The mouse maintains an accumulated absolute position. The driver has no
;; screen-size source, so it tracks within a fixed default space and clamps;
;; the compositor maps pointer coords as it likes.
(define PS2-SCREEN-W 1024)
(define PS2-SCREEN-H 768)
(define PS2-MAX-X (- PS2-SCREEN-W 1))   ; 1023
(define PS2-MAX-Y (- PS2-SCREEN-H 1))   ; 767

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

;; --- aux (mouse) device init -------------------------------------------------

;; Write one byte to the AUX device (port 2): every mouse-bound command must be
;; prefixed by controller command 0xD4 ("write next data byte to port 2"), else
;; it goes to the keyboard. ps2-write then drops the byte into 0x60.
(define (ps2-aux-write b)
  (ps2-cmd #xD4)
  (ps2-write b))

;; Send one mouse command and consume its 0xFA ACK. Returns #t if the device
;; ACK'd (0xFA), #f otherwise -- so the caller can bail out cleanly if no mouse
;; is present without hanging (the bounded ps2-read handles the missing-device
;; case by timing out to -1).
(define (ps2-aux-cmd b)
  (ps2-aux-write b)
  (= (ps2-read) #xFA))

;; Initialize the mouse on port 2: reset, set defaults, enable data reporting.
;; Each step is bounded and gated on the ACK; a missing/wedged mouse fails the
;; first ACK and we return #f (keyboard is unaffected). On 0xFF (reset) the
;; device replies 0xFA, then 0xAA (self-test pass), then 0x00 (device id) -- we
;; consume both extra bytes after the ACK.
;; After a reset (0xFF) the mouse runs its Battery-Acceptance-Test, which can take
;; up to ~500 ms on real hardware -- far longer than ps2-read's ~1 ms bounded spin.
;; Poll for the 0xAA self-test-pass byte with short sleeps so a slow mouse is not
;; falsely reported absent (and so its late 0xAA does not desync the F6/F4 ACKs).
(define (ps2-wait-bat)
  (let loop ((tries 0))
    (let ((b (ps2-read)))
      (cond ((= b #xAA) #t)
            ((>= tries 50) #f)               ; ~500 ms cap
            (else (sleep 10000000) (loop (+ tries 1)))))))

(define (ps2-mouse-init)
  (if (not (ps2-aux-cmd #xFF))               ; reset; expect ACK
      #f
      (begin
        (ps2-wait-bat)                       ; 0xAA self-test result (BAT-tolerant)
        (ps2-read)                           ; 0x00 device id
        (and (ps2-aux-cmd #xF6)              ; set defaults (sample 100Hz, res, scale 1:1)
             (ps2-aux-cmd #xF4)))))          ; enable data reporting

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
  (ps2-cmd #xA7)        ; disable port 2
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
          (ps2-cmd #xA8)                    ; enable port 2 (aux/mouse)
          (ps2-kbd-init)
          ;; Bring up the mouse on port 2. If it doesn't ACK (no mouse, wedged
          ;; controller, or QEMU not delivering aux input), log and continue --
          ;; the keyboard must still come up. mouse-up gates IRQ12 below.
          (let ((mouse-up (ps2-mouse-init)))
            (ps2-flush)
            ;; Re-read config and set: bit0 = keyboard IRQ (always), bit1 = aux
            ;; IRQ (only if the mouse came up), and clear bit5 (aux clock disable)
            ;; so port 2 is clocked. We deliberately leave the aux IRQ masked if the
            ;; mouse never initialized, so a phantom port 2 can't wedge IRQ12.
            (ps2-cmd #x20)
            (let* ((base (bitwise-and (ps2-read) (bitwise-not #x20))) ; clear aux-clock-disable
                   (cfg2 (bitwise-or base (if mouse-up (bitwise-or 1 2) 1))))
              (ps2-cmd #x60) (ps2-write cfg2))
            (ps2-flush)
            (display "[ps2] keyboard up") (newline)
            (if mouse-up
                (begin (display "[ps2] mouse up") (newline))
                (begin (display "[ps2] no mouse (aux init failed); keyboard only") (newline)))
            #t)))))

;; --- mouse packet decode (pure, host-testable) ------------------------------

;; Decode one standard 3-byte PS/2 mouse packet into a list
;;   (dx dy left? right? middle?)
;; where dx is the signed X movement, dy is the signed movement IN SCREEN SPACE
;; (PS/2 reports +Y up, screen is +Y down, so dy is NEGATED here), and the three
;; button flags are #t/#f.
;;
;; flags (byte0): bit0 left, bit1 right, bit2 middle, bit3 always-1,
;;                bit4 X-sign, bit5 Y-sign, bit6 X-overflow, bit7 Y-overflow.
;; The movement bytes are the low 8 bits of a 9-bit signed value whose sign bit
;; lives in byte0; reconstruct by subtracting 256 when the sign bit is set.
(define (ps2-mouse-decode flags b1 b2)
  (let ((dx (if (= 0 (bitwise-and flags #x10)) b1 (- b1 256)))
        (dy (if (= 0 (bitwise-and flags #x20)) b2 (- b2 256))))
    (list dx
          (- 0 dy)                                   ; PS/2 +Y up -> screen +Y down
          (not (= 0 (bitwise-and flags 1)))          ; left
          (not (= 0 (bitwise-and flags 2)))          ; right
          (not (= 0 (bitwise-and flags 4))))))       ; middle

;; A byte0 with bit3 ("always 1") clear is not a valid packet start -- the stream
;; has desynced. The drain loop drops such bytes until a valid byte0 appears.
(define (ps2-mouse-sync? flags)
  (not (= 0 (bitwise-and flags #x08))))

;; Clamp a value into [lo, hi].
(define (ps2-clamp v lo hi)
  (cond ((< v lo) lo)
        ((> v hi) hi)
        (else v)))

;; Accumulate a relative (dx,dy) onto an absolute (x,y), clamping to the default
;; pointer space. Returns (new-x new-y). Pure: the caller owns the state.
(define (ps2-mouse-accum x y dx dy)
  (list (ps2-clamp (+ x dx) 0 PS2-MAX-X)
        (ps2-clamp (+ y dy) 0 PS2-MAX-Y)))

;; --- mouse packet framing (driver state) ------------------------------------

;; The 3-byte packet is reassembled across IRQs. A small mutable vector holds the
;; whole pointer state so the pump can carry it without threading it through every
;; recursion: [0]=phase(0/1/2) [1]=byte0 [2]=byte1 [3]=abs-x [4]=abs-y [5]=left?.
;; Pointer starts at the centre of the default space.
(define (ps2-mouse-new-state)
  (let ((v (make-vector 6 0)))
    (vector-set! v 3 (quotient PS2-SCREEN-W 2))   ; centre x
    (vector-set! v 4 (quotient PS2-SCREEN-H 2))   ; centre y
    (vector-set! v 5 #f)                           ; left button up
    v))

;; Feed one mouse byte into the framing state. On phase 0 we validate the
;; always-1 bit (drop the byte on desync, staying in phase 0); phases 1 and 2
;; stash the movement bytes. On the 3rd byte we decode, update the accumulated
;; position + button, emit (event (pointer x y left?)) to coreinput, and reset to
;; phase 0.
(define (ps2-mouse-feed st b coreinput)
  (let ((phase (vector-ref st 0)))
    (cond
      ((= phase 0)
       (if (ps2-mouse-sync? b)
           (begin (vector-set! st 1 b) (vector-set! st 0 1))
           #f))                                     ; desync: drop, stay in phase 0
      ((= phase 1)
       (vector-set! st 2 b) (vector-set! st 0 2))
      (else                                         ; phase 2: packet complete
       (let* ((flags (vector-ref st 1))
              (dec (ps2-mouse-decode flags (vector-ref st 2) b))
              (dx (car dec)) (dy (cadr dec)) (left? (caddr dec))
              (pos (ps2-mouse-accum (vector-ref st 3) (vector-ref st 4) dx dy))
              (nx (car pos)) (ny (cadr pos)))
         (vector-set! st 3 nx)
         (vector-set! st 4 ny)
         (vector-set! st 5 left?)
         (vector-set! st 0 0)
         (send coreinput (list 'event (list 'pointer nx ny left?))))))))

;; --- the IRQ-driven scancode pump -------------------------------------------

;; Drain every byte currently in the output buffer. Keyboard bytes (status mouse
;; bit clear) forward to `coreinput` as (event (key <scancode> <pressed?>)); 0xFA
;; is the command ACK and is ignored; 0xF0 is the set-2 break prefix, so the
;; following byte is a release. Mouse bytes (status mouse bit set) feed the 3-byte
;; packet framer in `mstate`, which emits (event (pointer x y left?)) per packet.
(define (ps2-drain coreinput mstate)
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
              ;; mouse byte: feed the 3-byte framer, which emits a pointer event
              ;; on each complete packet.
              (ps2-mouse-feed mstate (in-u8 PS2-DATA) coreinput))
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
;; Wake cadence when the keyboard line is quiet. The i8042 multiplexes keyboard
;; (IRQ1) and mouse (IRQ12) onto one output buffer, but each line has its own
;; counter and only one waiter; an IRQ12 wake can't unpark a context parked on
;; IRQ1. So the pump parks on IRQ1 with a bounded timeout and re-drains the shared
;; port on wake OR timeout -- routing each byte by the status mouse bit. The
;; timeout caps mouse latency when there are no keystrokes (a moving-only mouse
;; reports at up to ~100 Hz; an 8 ms poll keeps the pointer smooth) without busy-
;; spinning. A keystroke still wakes IRQ1 immediately. We register IRQ12 too so the
;; aux line is routed to this core and counted, even though we don't park on it.
(define PS2-MOUSE-POLL-NS 8000000)        ; 8 ms

(define (ps2-keyboard-driver coreinput)
  (send coreinput (list 'register 'ps2-keyboard))
  (let ((irq (irq-register 1))             ; keyboard
        (mirq (irq-register 12))           ; mouse (aux): route + count the line
        (mstate (ps2-mouse-new-state)))
    (if (not irq)
        (begin (display "[ps2] irq-register failed") (newline) #f)
        (begin
          (ps2-irq-selftest irq)
          ;; `seen` is captured BEFORE draining so a key landing during the drain
          ;; advances the counter and makes irq-wait return immediately (re-drain)
          ;; instead of parking on a byte already in the buffer -- the count-based,
          ;; never-missed wake. Mouse bytes share the same buffer and are drained
          ;; in the same pass, routed by the status mouse bit.
          (let pump ((seen (irq-count irq)))
            (ps2-drain coreinput mstate)
            (if mirq
                (irq-wait irq seen PS2-MOUSE-POLL-NS)   ; bounded: re-drain for mouse too
                (irq-wait irq seen))                    ; no aux line: park on keyboard
            (pump (irq-count irq))))))))  ; last ) closes (define-module ps2 ...)
