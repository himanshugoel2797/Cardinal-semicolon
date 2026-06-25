;; usb-hid: the USB HID boot-protocol class driver (keyboard + mouse), ported
;; from drivers/usb_hid. It registers with coreusb as the class handler for
;; bInterfaceClass == HID; on (probe dev) it selects the boot protocol, registers
;; a keyboard with coreinput, and spawns a per-device context that polls the
;; interrupt-IN endpoint and decodes reports into coreinput events.
;;
;; The class-driver context and the per-device poll contexts hold NO hardware
;; capability -- all transfers are messages to the controller context (coreusb's
;; proto). usb-hid imports coreusb (the transfer API + constants) and driver-util.
;;
;; HOT-UNPLUG: remove sends the poll context (stop). Its transfer wait captures a
;; 'stop that arrives mid-transfer into a local flag (while still draining the
;; in-flight completion), so the stop is never dropped -- the poll context always
;; exits on unplug. STALL recovery: a run of failed interrupt transfers triggers
;; CLEAR_FEATURE(ENDPOINT_HALT) instead of spinning forever on a halted endpoint.
(define-module usb-hid
  (export usb-hid-init)
  (import coreusb driver-util)

  (define HID-PROTO-KEYBOARD 1)
  (define HID-PROTO-MOUSE    2)
  (define HID-REQ-SET-IDLE     #x0A)
  (define HID-REQ-SET-PROTOCOL #x0B)
  ;; class, interface-recipient request type for HID class requests (0x21).
  (define HID-IFACE-REQ (bitwise-or USB-REQ-TYPE-CLASS USB-REQ-RECIP-INTERFACE))

  (define (proto-name p)
    (cond ((= p HID-PROTO-KEYBOARD) "keyboard")
          ((= p HID-PROTO-MOUSE) "mouse")
          (else "hid")))

  ;; Is keycode k present in the boot-keyboard report's keycode slots (2..7)?
  (define (in-report? rpt k)
    (let loop ((j 2))
      (cond ((= j 8) #f) ((= (bytes-u8-ref rpt j) k) #t) (else (loop (+ j 1))))))

  ;; Diff a new boot-keyboard report against the previous one and emit key
  ;; down/up events to coreinput. rpt[0]=modifiers, rpt[2..7]=currently-down keys.
  (define (decode-keyboard rpt last input)
    (let loop ((i 2))                                   ; newly pressed
      (if (< i 8)
          (let ((k (bytes-u8-ref rpt i)))
            (if (and (not (= k 0)) (not (in-report? last k)))
                (begin (display "[usb-hid] key down usage=") (display k) (newline)
                       (send input (list 'event (list 'kbd-down k)))))
            (loop (+ i 1)))))
    (let loop ((i 2))                                   ; released
      (if (< i 8)
          (let ((k (bytes-u8-ref last i)))
            (if (and (not (= k 0)) (not (in-report? rpt k)))
                (send input (list 'event (list 'kbd-up k))))
            (loop (+ i 1))))))

  ;; Snapshot the 8-byte report so the next poll can diff against it.
  (define (snap8 rpt)
    (let ((o (make-bytes 8)))
      (let loop ((i 0)) (if (< i 8) (begin (bytes-u8-set! o i (bytes-u8-ref rpt i)) (loop (+ i 1))) o))))

  ;; Per-device poll loop (own context). Polls the interrupt-IN endpoint; a report
  ;; of >=8 bytes is decoded (keyboard). The 5ms transfer timeout paces this; a
  ;; small gap keeps a quiet keyboard from spinning.
  ;;
  ;; Stop is now race-free: remove sends (stop), and the transfer wait below
  ;; CAPTURES a 'stop that arrives WHILE a transfer is in flight into a local flag
  ;; (still draining the controller's in-flight completion) instead of dropping it
  ;; -- closing the old %mailbox-empty? race. A run of failed transfers (a stalled
  ;; endpoint) triggers CLEAR_FEATURE(ENDPOINT_HALT) so a stall is recovered
  ;; rather than spun on forever.
  (define (hid-poll dev ep mps proto input)
    (let ((stopped #f) (n (if (> mps 8) 8 mps)))
      (define (await)                 ; wait for the completion; note a 'stop
        (let ((m (recv)))
          (cond ((eq? (car m) 'complete) m)
                ((eq? (car m) 'stop) (set! stopped #t) (await))
                (else (await)))))
      (define (poll1)
        (send (usb-dev-hci dev)
              (list 'interrupt-in (usb-dev-address dev) (usb-dev-speed dev) ep n n (self)))
        (await))
      ;; A no-data control transfer through the SAME stop-aware await, so a 'stop
      ;; arriving during recovery isn't swallowed (proto's usb-control-* use a
      ;; await that drops non-completions -- unsafe to call while multiplexing
      ;; stop on this mailbox). ep0 max-packet 8 suffices for a zero-length xfer.
      (define (ctl0 setup)
        (send (usb-dev-hci dev)
              (list 'control (usb-dev-address dev) (usb-dev-speed dev) 8 setup #f 0 (self)))
        (await))
      (define (clear-halt)
        (display "[usb-hid] clearing endpoint halt") (newline)
        (ctl0 (make-setup USB-REQ-RECIP-ENDPOINT USB-REQ-CLEAR-FEATURE
                          USB-FEATURE-ENDPOINT-HALT ep 0)))
      (let loop ((last (make-bytes 8)) (fails 0))
        (if stopped 'stopped
            (let* ((c (poll1)) (nb (complete-n c)) (rpt (complete-data c)))
              (cond
                (stopped 'stopped)                 ; 'stop arrived during the poll
                ((and rpt (>= nb 0) (>= (bytes-length rpt) 8))
                 (if (= proto HID-PROTO-KEYBOARD) (decode-keyboard rpt last input))
                 (sleep 8000000) (loop (snap8 rpt) 0))
                ((< nb 0)                          ; transfer error -- likely a stall
                 (if (>= fails 2)
                     (begin (clear-halt) (sleep 8000000)
                            (if stopped 'stopped (loop last 0)))
                     (begin (sleep 8000000) (loop last (+ fails 1)))))
                (else (sleep 8000000) (loop last fails))))))))   ; NAK/short: keep polling

  ;; (probe dev): claim a HID device with an interrupt-IN endpoint, set boot
  ;; protocol + idle, register a keyboard with coreinput, and start its poll
  ;; context. Returns the new device list (addr . poll-ctx).
  (define (hid-on-probe dev input devs)
    (let ((ep (usb-find-endpoint dev USB-XFER-INTERRUPT #t)))
      (if (not ep)
          (begin (display "[usb-hid] no interrupt IN endpoint; not claiming") (newline) devs)
          (let* ((proto (usb-iface-protocol dev))
                 (iface (let ((i (usb-iface-number dev))) (if (< i 0) 0 i)))
                 (addr (usb-dev-address dev))
                 (epaddr (car ep))
                 (mps (let ((m (cadr ep))) (if (> m 0) m 8))))
            (display "[usb-hid] claimed ") (display (proto-name proto)) (newline)
            ;; SET_PROTOCOL boot (wValue 0) and SET_IDLE (wValue 0); best-effort.
            (usb-control-out dev HID-IFACE-REQ HID-REQ-SET-PROTOCOL 0 iface #f 0)
            (usb-control-out dev HID-IFACE-REQ HID-REQ-SET-IDLE 0 iface #f 0)
            (if (= proto HID-PROTO-KEYBOARD)
                (begin (send input (list 'register "USB Keyboard"))
                       (display "[usb-hid] registered keyboard with coreinput") (newline)))
            (let ((poll (spawn-restricted '()
                          (lambda () (hid-poll dev epaddr mps proto input)))))
              (display "[usb-hid] polling started") (newline)
              (cons (cons addr poll) devs))))))

  (define (hid-on-remove addr devs)
    (let loop ((ds devs) (keep '()))
      (cond ((null? ds) keep)
            ((= (caar ds) addr)
             (send (cdar ds) (list 'stop))    ; a list, so the poll's (car m) is valid
             (display "[usb-hid] device removed") (newline)
             (loop (cdr ds) keep))
            (else (loop (cdr ds) (cons (car ds) keep))))))

  ;; Entry point: init.clp calls (usb-hid-init usb input). Spawns the class-driver
  ;; context (state = active (addr . poll-ctx) list) and registers it with coreusb.
  (define (usb-hid-init usb input)
    (let ((ctx (serve '()
                 (lambda (devs m)
                   (cond ((eq? (car m) 'probe)  (hid-on-probe (cadr m) input devs))
                         ((eq? (car m) 'remove) (hid-on-remove (cadr m) devs))
                         (else devs))))))
      (send usb (list 'register-class USB-CLASS-HID ctx))
      ctx)))
