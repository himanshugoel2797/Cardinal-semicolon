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
;; LIMITATION (vs the C driver's stop/stopped handshake): remove asks a poll
;; context to exit by sending it 'stop, which it checks between polls. A 'stop
;; that races an in-flight transfer's completion can be missed; a real hot-unplug
;; is rare here and only leaves a poll context retrying failing transfers. Good
;; enough for now; a shared stop-cell would close it.
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
  ;; of >=8 bytes is decoded (keyboard) or logged. Exits when sent any message
  ;; (remove's 'stop). The 5ms transfer timeout paces this; add a small gap so a
  ;; quiet keyboard doesn't spin.
  (define (hid-poll dev ep mps proto input)
    (let ((n (if (> mps 8) 8 mps)))
      (let loop ((last (make-bytes 8)))
        (if (not (%mailbox-empty?))
            'stopped
            (let ((rpt (usb-interrupt-in dev ep n n)))
              (cond
                ((and rpt (>= (bytes-length rpt) 8))
                 (if (= proto HID-PROTO-KEYBOARD) (decode-keyboard rpt last input))
                 (sleep 8000000) (loop (snap8 rpt)))
                (else (sleep 8000000) (loop last))))))))

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
             (send (cdar ds) 'stop)
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
