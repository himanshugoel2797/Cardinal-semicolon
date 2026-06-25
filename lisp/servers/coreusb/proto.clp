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
(define USB-REQ-RECIP-DEVICE    #x00)
(define USB-REQ-RECIP-INTERFACE #x01)
(define USB-REQ-RECIP-ENDPOINT  #x02)
(define USB-REQ-RECIP-OTHER     #x03)

(define USB-REQ-GET-STATUS        0)
(define USB-REQ-CLEAR-FEATURE     1)
(define USB-REQ-SET-FEATURE       3)
(define USB-REQ-SET-ADDRESS       5)
(define USB-REQ-GET-DESCRIPTOR    6)
(define USB-REQ-SET-CONFIGURATION 9)
(define USB-REQ-SET-INTERFACE     11)

;; CLEAR_FEATURE / SET_FEATURE selectors.
(define USB-FEATURE-ENDPOINT-HALT 0)

(define USB-DESC-DEVICE     1)
(define USB-DESC-CONFIG     2)
(define USB-DESC-STRING     3)
(define USB-DESC-INTERFACE  4)
(define USB-DESC-ENDPOINT   5)
(define USB-DESC-IFACE-ASSOC 11)   ; Interface Association Descriptor (multi-fn)

(define USB-CLASS-AUDIO        #x01)
(define USB-CLASS-CDC          #x02)
(define USB-CLASS-HID          #x03)
(define USB-CLASS-MASS-STORAGE #x08)
(define USB-CLASS-HUB          #x09)

;; transfer types in bmAttributes bits1:0
(define USB-XFER-CONTROL   0)
(define USB-XFER-ISOCH     1)
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

;; Hub-driver helpers. mark-hub goes to the controller (tells it the device is a
;; hub so it routes to devices behind it; a no-op reply on UHCI). enumerate- /
;; disconnect-downstream go to the COREUSB service (`usb`), which spawns/tears
;; down an enumerator for the downstream port with this hub as the parent.
(define (usb-mark-hub dev nports)
  (complete-n (hci-mark-hub (usb-dev-hci dev) (usb-dev-address dev) nports)))
(define (usb-enumerate-downstream usb dev port speed)
  (send usb (list 'enumerate-downstream (usb-dev-hci dev) (usb-dev-address dev) port speed)))
(define (usb-disconnect-downstream usb dev port)
  (send usb (list 'disconnect-downstream (usb-dev-hci dev) (usb-dev-address dev) port)))

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
(define (usb-iface-subclass dev) (usb-iface-field dev 6))
(define (usb-iface-protocol dev) (usb-iface-field dev 7))
(define (usb-iface-number dev)   (usb-iface-field dev 2))

;; ---- full descriptor model (multi-interface / alt-setting aware) ------------
;; The accessors above answer the FIRST interface only -- enough for HID/storage,
;; which are single-interface. Audio (and other multi-function classes) need to
;; see every interface, its alternate settings, and the endpoints scoped to a
;; given (interface,alt). These walkers parse the whole configuration blob.

;; Interface (alt-setting) descriptor record + accessors.
;;   (number alt class subclass protocol num-endpoints byte-offset)
(define (mk-iface n alt cls sub proto neps off) (list n alt cls sub proto neps off))
(define (iface-number i)   (car i))
(define (iface-alt i)      (cadr i))
(define (iface-class i)    (caddr i))
(define (iface-subclass i) (cadddr i))
(define (iface-protocol i) (nth i 4))
(define (iface-num-eps i)  (nth i 5))
(define (iface-offset i)   (nth i 6))

;; Every interface descriptor (one record per alternate setting) in order.
(define (usb-interfaces dev)
  (let ((cfg (usb-dev-config dev)) (clen (usb-dev-config-len dev)))
    (let loop ((off 0) (acc '()))
      (if (> (+ off 2) clen)
          (reverse acc)
          (let ((len (bytes-u8-ref cfg off)) (dtype (bytes-u8-ref cfg (+ off 1))))
            (cond
              ((= len 0) (reverse acc))
              ((and (= dtype USB-DESC-INTERFACE) (>= len 9) (<= (+ off 9) clen))
               (loop (+ off len)
                     (cons (mk-iface (bytes-u8-ref cfg (+ off 2)) (bytes-u8-ref cfg (+ off 3))
                                     (bytes-u8-ref cfg (+ off 5)) (bytes-u8-ref cfg (+ off 6))
                                     (bytes-u8-ref cfg (+ off 7)) (bytes-u8-ref cfg (+ off 4))
                                     off)
                           acc)))
              (else (loop (+ off len) acc))))))))

;; Endpoint descriptor record + accessors.
;;   (address bmAttributes max-packet bInterval)
(define (mk-ep addr attr mps ival) (list addr attr mps ival))
(define (ep-address e)    (car e))
(define (ep-attributes e) (cadr e))
(define (ep-type e)       (bitwise-and (cadr e) 3))      ; 0=ctl 1=iso 2=bulk 3=int
(define (ep-sync-type e)  (bitwise-and (arithmetic-shift (cadr e) -2) 3)) ; iso sync
(define (ep-dir-in? e)    (not (= 0 (bitwise-and (car e) #x80))))
(define (ep-number e)     (bitwise-and (car e) #xF))
(define (ep-max-packet e) (caddr e))
(define (ep-interval e)   (cadddr e))

;; The endpoint descriptors belonging to interface `num` alternate setting `alt`:
;; those between that interface descriptor and the next interface descriptor.
;; Returns a list of endpoint records (possibly empty -- alt 0 of an audio
;; streaming interface has zero endpoints).
(define (usb-iface-endpoints dev num alt)
  (let ((cfg (usb-dev-config dev)) (clen (usb-dev-config-len dev)))
    (let scan ((off 0))
      (if (> (+ off 2) clen)
          '()
          (let ((len (bytes-u8-ref cfg off)) (dtype (bytes-u8-ref cfg (+ off 1))))
            (cond
              ((= len 0) '())
              ((and (= dtype USB-DESC-INTERFACE) (>= len 9)
                    (= (bytes-u8-ref cfg (+ off 2)) num)
                    (= (bytes-u8-ref cfg (+ off 3)) alt))
               (let collect ((o (+ off len)) (acc '()))
                 (if (> (+ o 2) clen)
                     (reverse acc)
                     (let ((l (bytes-u8-ref cfg o)) (dt (bytes-u8-ref cfg (+ o 1))))
                       (cond
                         ((= l 0) (reverse acc))
                         ((= dt USB-DESC-INTERFACE) (reverse acc))   ; next iface: stop
                         ((and (= dt USB-DESC-ENDPOINT) (>= l 7) (<= (+ o 7) clen))
                          (collect (+ o l)
                                   (cons (mk-ep (bytes-u8-ref cfg (+ o 2)) (bytes-u8-ref cfg (+ o 3))
                                                (bytes-u16-ref cfg (+ o 4)) (bytes-u8-ref cfg (+ o 6)))
                                         acc)))
                         (else (collect (+ o l) acc)))))))   ; skip class-specific descriptors
              (else (scan (+ off len)))))))))

;; First endpoint of transfer `type` / direction `dir-in?` in an endpoint list.
(define (usb-find-ep-in eps type dir-in?)
  (cond ((null? eps) #f)
        ((and (= (ep-type (car eps)) type) (eq? (ep-dir-in? (car eps)) dir-in?)) (car eps))
        (else (usb-find-ep-in (cdr eps) type dir-in?))))

;; ---- standard requests (class-driver-facing) --------------------------------
;; Generic GET_DESCRIPTOR with explicit recipient + wIndex; returns the data
;; bytevector or #f. For a device-level descriptor (BOS, ...) pass
;; USB-REQ-RECIP-DEVICE and wIndex 0; for a HID report descriptor pass
;; USB-REQ-RECIP-INTERFACE and the interface number as wIndex (the interface
;; recipient is required there -- a device-recipient request would STALL).
(define (usb-get-descriptor dev recip dtype index windex len)
  (usb-control-in dev recip USB-REQ-GET-DESCRIPTOR
                  (bitwise-or (arithmetic-shift dtype 8) index) windex len))

;; SET_INTERFACE: select alternate setting `alt` of interface `iface`. Audio uses
;; this to switch a streaming interface from its zero-bandwidth alt 0 to an alt
;; that exposes the isochronous endpoint. Returns the control byte count (>=0) or -1.
(define (usb-set-interface dev iface alt)
  (usb-control-out dev USB-REQ-RECIP-INTERFACE USB-REQ-SET-INTERFACE alt iface #f 0))

;; ---- robustness: retry + endpoint-halt recovery -----------------------------
;; Run `thunk` up to `tries` times, returning the first result that satisfies
;; `ok?`; on a failing result, nap `gap-ns` and retry, returning the last result
;; once tries are exhausted. The control transfers the enumerator issues are
;; one-shot and failure-prone on real hardware (a NAK storm, a slow device), and
;; the USB spec already expects a host to make several attempts -- so the
;; enumerator wraps each in this. `ok?` is a predicate over the thunk's result.
(define (with-retries tries gap-ns ok? thunk)
  (let loop ((n tries))
    (let ((r (thunk)))
      (if (or (ok? r) (<= n 1)) r (begin (sleep gap-ns) (loop (- n 1)))))))

;; CLEAR_FEATURE(ENDPOINT_HALT): clear a stalled bulk/interrupt endpoint so the
;; device can resume on it (also resets the endpoint's data toggle on the device;
;; the controller tracks its own toggle, so a class driver that clears a halt
;; mid-stream may also need to reset its side -- both UHCI and xHCI reset the
;; toggle on the next configure/transfer here). `ep-addr` is the full endpoint
;; address (direction bit included). Returns the control byte count (>=0) or -1.
;; CAVEAT: rides usb-control-out -> await-complete, which DROPS non-completion
;; messages. A context multiplexing other messages on its mailbox (a poll loop
;; watching for 'stop, a server draining requests) must issue the CLEAR_FEATURE
;; through its OWN message wait instead, or that message is lost (see usb-hid's
;; clear-halt). Safe from a context that only awaits this reply.
(define (usb-clear-halt dev ep-addr)
  (usb-control-out dev USB-REQ-RECIP-ENDPOINT USB-REQ-CLEAR-FEATURE
                   USB-FEATURE-ENDPOINT-HALT ep-addr #f 0))

;; ---- string descriptors -----------------------------------------------------
;; Raw GET_DESCRIPTOR(STRING,index) bytes (<=255), or #f. langid 0/index 0 yields
;; the supported-LANGID array. We over-read (255) and trust bLength via the data.
(define (usb-string-raw dev index langid)
  (let ((c (hci-control (usb-dev-hci dev) (usb-dev-address dev) (usb-dev-speed dev)
                        (usb-dev-mps0 dev)
                        (make-setup (bitwise-or USB-REQ-RECIP-DEVICE USB-REQ-DIR-IN)
                                    USB-REQ-GET-DESCRIPTOR
                                    (bitwise-or (arithmetic-shift USB-DESC-STRING 8) index)
                                    langid 255)
                        #f 255)))
    (if (< (complete-n c) 2) #f (complete-data c))))

;; The device's first supported LANGID (e.g. 0x0409 en-US), or 0 if none.
(define (usb-langid dev)
  (let ((s (usb-string-raw dev 0 0)))
    (if (and s (>= (bytes-length s) 4)) (bytes-u16-ref s 2) 0)))

;; Decode a UTF-16LE string descriptor's bytes (after the 2-byte header) into an
;; ASCII Lisp string; non-ASCII code units fold to '?'. Pure (host-testable).
(define (usb-string-decode s)
  (let ((n (bytes-length s)))
    (let loop ((i 2) (acc '()))
      (if (> (+ i 2) n)
          (list->string (reverse acc))
          (let ((u (bytes-u16-ref s i)))
            (loop (+ i 2) (cons (integer->char (if (< u 128) u 63)) acc)))))))

;; Fetch + decode string `index` (in `langid`); "" for index 0 or on failure.
(define (usb-string dev index langid)
  (if (= index 0) ""
      (let ((s (usb-string-raw dev index langid)))
        (if s (usb-string-decode s) ""))))
