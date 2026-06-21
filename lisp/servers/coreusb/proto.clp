;; coreusb/proto -- USB wire constants, the setup-packet builder, and the
;; host-controller transfer protocol shared by the enumerator and class drivers.
;;
;; THE TRANSFER MODEL (the Lisp analogue of usb_hci_handlers_t in CoreUsb/usb.h).
;; A host controller is a long-lived CONTEXT that owns its hardware + DMA buffers
;; and answers transfer-request messages. Every interaction is a message to that
;; context handle (`hci`), and every request carries a reply context as its last
;; element; the controller answers with (complete <n> <data-or-#f>) where <n> is
;; the byte count transferred (negative on error/timeout/STALL) and <data> is a
;; fresh bytevector for IN transfers (copy-on-send hands the caller its own copy),
;; or #f for OUT/no-data. The synchronous C handlers become these messages:
;;
;;   (control       addr speed mps setup data len reply) -> (complete n data|#f)
;;   (interrupt-in  addr speed ep maxp len reply)         -> (complete n data)
;;   (bulk          addr ep maxp data len dir-in? reply)  -> (complete n data|#f)
;;   (prepare-downstream parent-addr port speed reply)    -> (complete status #f)
;;   (mark-hub      addr nports reply)                    -> (complete status #f)
;;   (disconnect-dev addr)                                -- fire-and-forget
;;
;; control direction lives in setup[0] bit7 (USB-REQ-DIR-IN); `data` is the OUT
;; payload (or #f) and `len` is wLength. prepare-downstream/mark-hub are optional
;; (xHCI implements them; UHCI replies (complete 0 #f) as a no-op).

;; ---- USB 2.0 ch.9 constants (mirrors servers/inc/CoreUsb/usb.h) -------------
(define USB-REQ-DIR-IN  #x80)
(define USB-REQ-DIR-OUT #x00)
(define USB-REQ-TYPE-CLASS #x20)
(define USB-REQ-RECIP-INTERFACE #x01)
(define USB-REQ-RECIP-OTHER     #x03)

(define USB-REQ-GET-STATUS        0)
(define USB-REQ-CLEAR-FEATURE     1)
(define USB-REQ-SET-FEATURE       3)
(define USB-REQ-SET-ADDRESS       5)
(define USB-REQ-GET-DESCRIPTOR    6)
(define USB-REQ-SET-CONFIGURATION 9)
(define USB-REQ-SET-INTERFACE     11)

(define USB-DESC-DEVICE    1)
(define USB-DESC-CONFIG    2)
(define USB-DESC-STRING    3)
(define USB-DESC-INTERFACE 4)
(define USB-DESC-ENDPOINT  5)

(define USB-CLASS-HID          #x03)
(define USB-CLASS-MASS-STORAGE #x08)
(define USB-CLASS-HUB          #x09)

;; transfer types in bmAttributes bits1:0
(define USB-XFER-BULK      2)
(define USB-XFER-INTERRUPT 3)

;; usb_speed_t
(define USB-SPEED-LOW   0)
(define USB-SPEED-FULL  1)
(define USB-SPEED-HIGH  2)
(define USB-SPEED-SUPER 3)

;; ---- setup packet (8 bytes, little-endian) ----------------------------------
(define (make-setup bmRequestType bRequest wValue wIndex wLength)
  (let ((s (make-bytes 8)))
    (bytes-u8-set!  s 0 bmRequestType)
    (bytes-u8-set!  s 1 bRequest)
    (bytes-u16-set! s 2 wValue)
    (bytes-u16-set! s 4 wIndex)
    (bytes-u16-set! s 6 wLength)
    s))

;; ---- low-level transfer round-trips to a host-controller context ------------
;; Send a request (appending the running context as the reply target) and wait
;; for its (complete n data) answer. await-complete DROPS any non-completion
;; message: the contexts that use these (the enumerator, HID/hub poll loops) only
;; ever receive transfer replies, so this is safe and keeps a stray wake from
;; being mistaken for a completion. usb-storage, which interleaves block requests
;; with transfers, runs its own stashing loop instead (see usb-storage).
(define (await-complete)
  (let ((m (recv)))
    (if (eq? (car m) 'complete) m (await-complete))))

(define (hci-control hci addr speed mps setup data len)
  (send hci (list 'control addr speed mps setup data len (self)))
  (await-complete))                 ; -> (complete n data|#f)

(define (hci-interrupt-in hci addr speed ep maxp len)
  (send hci (list 'interrupt-in addr speed ep maxp len (self)))
  (await-complete))

(define (hci-bulk hci addr ep maxp data len dir-in?)
  (send hci (list 'bulk addr ep maxp data len dir-in? (self)))
  (await-complete))

(define (hci-prepare-downstream hci parent-addr port speed)
  (send hci (list 'prepare-downstream parent-addr port speed (self)))
  (await-complete))

(define (hci-mark-hub hci addr nports)
  (send hci (list 'mark-hub addr nports (self)))
  (await-complete))

;; completion accessors
(define (complete-n c)    (cadr c))
(define (complete-data c) (caddr c))

;; ---- the enumerated-device value handed to a class driver -------------------
;; Plain data + the owning controller's context handle; a class driver issues
;; transfers by sending to (usb-dev-hci dev). config is the raw configuration
;; descriptor bytevector (interfaces + endpoints), parsed by usb-find-endpoint.
;;   (hci address speed max-packet0 config-bytes config-len)
(define (make-usb-dev hci address speed mps0 config len)
  (list hci address speed mps0 config len))
(define (usb-dev-hci    d) (car d))
(define (usb-dev-address d) (cadr d))
(define (usb-dev-speed  d) (caddr d))
(define (usb-dev-mps0   d) (cadddr d))
(define (usb-dev-config d) (nth d 4))
(define (usb-dev-config-len d) (nth d 5))

;; ---- class-driver-facing transfer API (runs in the class driver's context) --
;; usb-control-in returns the data bytevector (length <= len) or #f on error;
;; usb-control-out returns the byte count (>=0) or -1.
(define (usb-control-in dev bmReq bReq wValue wIndex len)
  (let ((c (hci-control (usb-dev-hci dev) (usb-dev-address dev) (usb-dev-speed dev)
                        (usb-dev-mps0 dev)
                        (make-setup (bitwise-or bmReq USB-REQ-DIR-IN) bReq wValue wIndex len)
                        #f len)))
    (if (< (complete-n c) 0) #f (complete-data c))))

(define (usb-control-out dev bmReq bReq wValue wIndex data len)
  (complete-n (hci-control (usb-dev-hci dev) (usb-dev-address dev) (usb-dev-speed dev)
                           (usb-dev-mps0 dev)
                           (make-setup (bitwise-or bmReq USB-REQ-DIR-OUT) bReq wValue wIndex len)
                           data len)))

(define (usb-interrupt-in dev endpoint max-packet len)
  (let ((c (hci-interrupt-in (usb-dev-hci dev) (usb-dev-address dev) (usb-dev-speed dev)
                             endpoint max-packet len)))
    (if (< (complete-n c) 0) #f (complete-data c))))

(define (usb-bulk-in dev endpoint max-packet len)
  (let ((c (hci-bulk (usb-dev-hci dev) (usb-dev-address dev) endpoint max-packet
                     #f len #t)))
    (if (< (complete-n c) 0) #f (complete-data c))))

(define (usb-bulk-out dev endpoint max-packet data len)
  (complete-n (hci-bulk (usb-dev-hci dev) (usb-dev-address dev) endpoint max-packet
                        data len #f)))

;; Find the first endpoint of a given transfer type (2=bulk,3=interrupt) and
;; direction (dir-in? = IN) in the configuration descriptor. Returns
;; (list ep-address max-packet) or #f. Walks the descriptor chain by bLength.
(define (usb-find-endpoint dev type dir-in?)
  (let ((cfg (usb-dev-config dev)) (clen (usb-dev-config-len dev)))
    (let loop ((off 0))
      (if (> (+ off 2) clen)
          #f
          (let ((len (bytes-u8-ref cfg off))
                (dtype (bytes-u8-ref cfg (+ off 1))))
            (cond
              ((= len 0) #f)
              ((and (= dtype USB-DESC-ENDPOINT) (<= (+ off 7) clen)
                    (= (bitwise-and (bytes-u8-ref cfg (+ off 3)) 3) type)
                    (eq? (not (= 0 (bitwise-and (bytes-u8-ref cfg (+ off 2)) #x80)))
                         (if dir-in? #t #f)))
               (list (bytes-u8-ref cfg (+ off 2))
                     (bytes-u16-ref cfg (+ off 4))))
              (else (loop (+ off len)))))))))

;; First interface descriptor's fields (bInterfaceClass / Protocol / Number), or
;; -1 if no interface descriptor is present.
(define (usb-iface-field dev which)
  (let ((cfg (usb-dev-config dev)) (clen (usb-dev-config-len dev)))
    (let loop ((off 0))
      (if (> (+ off 2) clen)
          -1
          (let ((len (bytes-u8-ref cfg off))
                (dtype (bytes-u8-ref cfg (+ off 1))))
            (cond
              ((= len 0) -1)
              ((and (= dtype USB-DESC-INTERFACE) (<= (+ off 9) clen))
               (bytes-u8-ref cfg (+ off which)))
              (else (loop (+ off len)))))))))
(define (usb-iface-class dev)    (usb-iface-field dev 5))
(define (usb-iface-protocol dev) (usb-iface-field dev 7))
(define (usb-iface-number dev)   (usb-iface-field dev 2))
