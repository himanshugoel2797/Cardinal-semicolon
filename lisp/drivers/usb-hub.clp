;; usb-hub: the USB hub class driver, ported from drivers/usb_hub. Registers with
;; coreusb as the handler for bInterfaceClass == Hub; on (probe dev) it reads the
;; hub descriptor, marks the device a hub (so the controller routes to devices
;; behind it), powers the ports, and spawns a poll context that resets + asks
;; coreusb to enumerate each newly-connected downstream device (which then routes
;; to its own class driver).
;;
;; Like usb-hid, the class-driver + poll contexts hold no hardware capability:
;; control transfers are messages to the controller, and downstream enumeration
;; is a message to the coreusb service (`usb`).
(define-module usb-hub
  (export usb-hub-init)
  (import coreusb driver-util)
  (define lg (make-logger 'usb-hub))

  (define HUB-DESC-TYPE #x29)
  (define HUB-MAX-PORTS 15)
  ;; hub class features
  (define PORT-RESET 4)
  (define PORT-POWER 8)
  (define C-PORT-CONNECTION 16)
  (define C-PORT-RESET 20)
  ;; wPortStatus bits
  (define PS-CONNECTION 1)
  (define PS-ENABLE     2)
  (define PS-LOW-SPEED  #x200)   ; bit 9
  ;; request types: 0xA3 = class|other-recipient|IN (GET_STATUS port),
  ;; 0x23 = class|other-recipient|OUT (SET/CLEAR_FEATURE port), 0x20 = class|device.
  (define REQ-CLASS-OTHER  (bitwise-or USB-REQ-TYPE-CLASS USB-REQ-RECIP-OTHER))
  (define REQ-CLASS-DEVICE USB-REQ-TYPE-CLASS)

  ;; Returns wPortStatus (low 16 bits) or -1 on failure.
  (define (hub-port-status dev port)
    (let ((b (usb-control-in dev REQ-CLASS-OTHER USB-REQ-GET-STATUS 0 port 4)))
      (if (and b (>= (bytes-length b) 2)) (bytes-u16-ref b 0) -1)))
  (define (hub-set-feature dev port feature)
    (usb-control-out dev REQ-CLASS-OTHER USB-REQ-SET-FEATURE feature port #f 0))
  (define (hub-clear-feature dev port feature)
    (usb-control-out dev REQ-CLASS-OTHER USB-REQ-CLEAR-FEATURE feature port #f 0))

  ;; Poll each downstream port: on a new connect, reset it then ask coreusb to
  ;; enumerate the device behind it; on a new disconnect, ask coreusb to tear it
  ;; down. `enumed` is a per-port "already enumerated?" list (1-based, so a head
  ;; placeholder at index 0). Exits when sent any message (remove's 'stop).
  (define (hub-poll usb dev nports)
    (let ((enumed (make-bytes (+ nports 1))))     ; 1 byte per port, 1-based
      (let loop ()
        (if (not (%mailbox-empty?))
            'stopped
            (begin
              (let ploop ((port 1))
                (if (<= port nports)
                    (let ((st (hub-port-status dev port)))
                      (if (>= st 0)
                          (let ((conn (not (= 0 (bitwise-and st PS-CONNECTION))))
                                (was (not (= 0 (bytes-u8-ref enumed port)))))
                            (cond
                              ((and conn (not was))
                               (lg "downstream connect, port " port)
                               (bytes-u8-set! enumed port 1)
                               (hub-clear-feature dev port C-PORT-CONNECTION)
                               (hub-set-feature dev port PORT-RESET)
                               (sleep 50000000)
                               (hub-clear-feature dev port C-PORT-RESET)
                               (sleep 20000000)
                               (let ((st2 (hub-port-status dev port)))
                                 (if (and (>= st2 0) (not (= 0 (bitwise-and st2 PS-ENABLE))))
                                     (usb-enumerate-downstream usb dev port
                                       (if (not (= 0 (bitwise-and st2 PS-LOW-SPEED)))
                                           USB-SPEED-LOW USB-SPEED-FULL)))))
                              ((and (not conn) was)
                               (lg "downstream disconnect, port " port)
                               (bytes-u8-set! enumed port 0)
                               (usb-disconnect-downstream usb dev port)))))
                      (ploop (+ port 1)))))
              (sleep 200000000)                    ; 200ms downstream poll cadence
              (loop))))))

  (define (hub-on-probe usb dev devs)
    (let ((desc (usb-control-in dev REQ-CLASS-DEVICE USB-REQ-GET-DESCRIPTOR
                                (arithmetic-shift HUB-DESC-TYPE 8) 0 8)))
      (if (or (not desc) (< (bytes-length desc) 3))
          (begin (lg "hub descriptor read failed; not claiming") devs)
          (let ((nports (let ((n (bytes-u8-ref desc 2))) (if (> n HUB-MAX-PORTS) HUB-MAX-PORTS n)))
                (addr (usb-dev-address dev)))
            (lg "claimed hub with " nports " ports")
            (usb-mark-hub dev nports)
            (let pp ((port 1)) (if (<= port nports) (begin (hub-set-feature dev port PORT-POWER) (pp (+ port 1)))))
            (sleep 100000000)                      ; port power-good settle
            (let ((poll (spawn-restricted '() (lambda () (hub-poll usb dev nports)))))
              (cons (cons addr poll) devs))))))

  (define (hub-on-remove addr devs)
    (let loop ((ds devs) (keep '()))
      (cond ((null? ds) keep)
            ((= (caar ds) addr)
             (send (cdar ds) 'stop)
             (lg "hub removed")
             (loop (cdr ds) keep))
            (else (loop (cdr ds) (cons (car ds) keep))))))

  ;; init.clp calls (usb-hub-init usb). The class context closes over the coreusb
  ;; handle so its poll contexts can request downstream enumeration.
  (define (usb-hub-init usb)
    (let ((ctx (serve '()
                 (lambda (devs m)
                   (cond ((eq? (car m) 'probe)  (hub-on-probe usb (cadr m) devs))
                         ((eq? (car m) 'remove) (hub-on-remove (cadr m) devs))
                         (else devs))))))
      (send usb (list 'register-class USB-CLASS-HUB ctx))
      ctx)))
