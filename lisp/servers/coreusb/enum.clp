;; coreusb/enum -- the controller-agnostic USB enumeration state machine and the
;; coreusb registry service. Ported from servers/CoreUsb/src/enum.c.
;;
;; The C server drove a device through address assignment + descriptor reads using
;; the host controller's synchronous transfer handlers, then dispatched to a class
;; driver registered for the device's interface class. Here the same logic runs in
;; a transient enumerator CONTEXT spawned per port-connect: it talks to the host
;; controller by message (proto.clp) and to class drivers by message, so nothing
;; calls across contexts except via handles.
;;
;; coreusb itself is a `serve` registry context owning three things the C globals
;; held: the class-driver table (class byte -> class context), the USB bus-address
;; pool (1..127; 0 is the default address), and the enumerated-device records used
;; to correlate a later disconnect back to a device. Protocol (send to the handle):
;;   (register-class <class-byte> <class-ctx>)
;;   (port-connected <hci-ctx> <port> <speed>)         ; root-port attach
;;   (port-disconnected <hci-ctx> <port>)              ; root-port detach
;;   (enumerate-downstream <hci-ctx> <parent-addr> <port> <speed>)   ; hub driver
;;   (disconnect-downstream <hci-ctx> <parent-addr> <port>)          ; hub driver
;; and, internally, the enumerator reports (enum-done ...) / (enum-failed <addr>).

;; ---- USB bus-address pool (1..127) -----------------------------------------
;; `used` is a list of in-use addresses; alloc hands out the lowest free one, so
;; a freed address is reused (matching alloc_address's lowest-free scan).
(define (addr-alloc used)
  (let loop ((a 1))
    (cond ((> a 127) #f)
          ((member? a used) (loop (+ a 1)))
          (else a))))
(define (member? x lst)
  (cond ((null? lst) #f) ((eq? (car lst) x) #t) (else (member? x (cdr lst)))))
(define (remove-elt x lst)
  (cond ((null? lst) '())
        ((eq? (car lst) x) (cdr lst))
        (else (cons (car lst) (remove-elt x (cdr lst))))))

;; ---- the enumeration body (runs in a spawned enumerator context) ------------
;; Drives the device at default address 0 to `addr`, reads its descriptors,
;; selects its configuration, and dispatches it to the matching class context.
;; `class-table` is a snapshot alist (class-byte . class-ctx); `usb` is the
;; coreusb handle to report the outcome to. parent-addr is 0 for a root port.
(define (enumerate hci port speed parent addr class-table usb)
  ;; 1) 8-byte device descriptor at the default address -> max packet size (ep0).
  (let ((c (hci-control hci 0 speed 8
                        (make-setup USB-REQ-DIR-IN USB-REQ-GET-DESCRIPTOR
                                    (arithmetic-shift USB-DESC-DEVICE 8) 0 8)
                        #f 8)))
    (if (< (complete-n c) 8)
        (begin (display "[coreusb] initial GET_DESCRIPTOR failed") (newline)
               (send usb (list 'enum-failed addr)))
        (let* ((d8 (complete-data c))
               (mps0 (let ((m (bytes-u8-ref d8 7))) (if (= m 0) 8 m))))
          ;; 2) SET_ADDRESS (still at the default address), then the recovery delay.
          (if (< (complete-n
                  (hci-control hci 0 speed mps0
                               (make-setup USB-REQ-DIR-OUT USB-REQ-SET-ADDRESS addr 0 0)
                               #f 0)) 0)
              (begin (display "[coreusb] SET_ADDRESS failed") (newline)
                     (send usb (list 'enum-failed addr)))
              (begin
                (sleep 50000000)              ; >=2ms by spec; xHCI wants more (50ms)
                (enumerate-stage2 hci port speed parent addr mps0 class-table usb)))))))

(define (enumerate-stage2 hci port speed parent addr mps0 class-table usb)
  ;; 3) full (18-byte) device descriptor at the new address.
  (let ((c (hci-control hci addr speed mps0
                        (make-setup USB-REQ-DIR-IN USB-REQ-GET-DESCRIPTOR
                                    (arithmetic-shift USB-DESC-DEVICE 8) 0 18)
                        #f 18)))
    (if (< (complete-n c) 18)
        (begin (display "[coreusb] device descriptor read failed") (newline)
               (send usb (list 'enum-failed addr)))
        (let* ((dd (complete-data c))
               (vid (bytes-u16-ref dd 8)) (pid (bytes-u16-ref dd 10))
               (dclass (bytes-u8-ref dd 4)))
          (display "[coreusb] enumerated device: vid=") (display vid)
          (display " pid=") (display pid) (display " class=") (display dclass) (newline)
          ;; 4) config descriptor: 9-byte header for wTotalLength, then the whole.
          (let ((ch (hci-control hci addr speed mps0
                                 (make-setup USB-REQ-DIR-IN USB-REQ-GET-DESCRIPTOR
                                             (arithmetic-shift USB-DESC-CONFIG 8) 0 9)
                                 #f 9)))
            (if (< (complete-n ch) 9)
                (begin (display "[coreusb] config header read failed") (newline)
                       (send usb (list 'enum-failed addr)))
                (let* ((total0 (bytes-u16-ref (complete-data ch) 2))
                       (total (if (> total0 512) 512 total0))
                       (cf (hci-control hci addr speed mps0
                                        (make-setup USB-REQ-DIR-IN USB-REQ-GET-DESCRIPTOR
                                                    (arithmetic-shift USB-DESC-CONFIG 8) 0 total)
                                        #f total)))
                  (if (< (complete-n cf) total)
                      (begin (display "[coreusb] config (full) read failed") (newline)
                             (send usb (list 'enum-failed addr)))
                      (let* ((config (complete-data cf))
                             (cfgval (bytes-u8-ref config 5))
                             (dev (make-usb-dev hci addr speed mps0 config total)))
                        ;; 5) select the configuration.
                        (if (< (complete-n
                                (hci-control hci addr speed mps0
                                             (make-setup USB-REQ-DIR-OUT USB-REQ-SET-CONFIGURATION
                                                         cfgval 0 0) #f 0)) 0)
                            (begin (display "[coreusb] SET_CONFIGURATION failed") (newline)
                                   (send usb (list 'enum-failed addr)))
                            ;; 6) advisory: log device strings, then dispatch by
                            ;; class (interface class if present, else device).
                            (begin
                            (log-device-strings dev dd)
                            (let* ((iclass (usb-iface-class dev))
                                   (klass (if (>= iclass 0) iclass dclass))
                                   (cdrv (assq-ctx klass class-table)))
                              (if cdrv
                                  (begin
                                    (display "[coreusb] dispatch to class ") (display klass) (newline)
                                    (send cdrv (list 'probe dev))
                                    (send usb (list 'enum-done addr hci port parent klass)))
                                  (begin
                                    (display "[coreusb] no class driver for class ")
                                    (display klass) (newline)
                                    (send usb (list 'enum-failed addr))))))))))))))))

;; Best-effort: read the device's manufacturer/product strings and log them, plus
;; note a multi-configuration device. Never fails enumeration -- string reads are
;; advisory. `dd` is the 18-byte device descriptor (iManufacturer=14, iProduct=15,
;; iSerial=16, bNumConfigurations=17).
(define (log-device-strings dev dd)
  (let ((nconf (bytes-u8-ref dd 17))
        (iman (bytes-u8-ref dd 14)) (iprod (bytes-u8-ref dd 15)))
    (if (> nconf 1)
        (begin (display "[coreusb]   ") (display nconf)
               (display " configurations present (using configuration 0)") (newline)))
    (if (or (> iman 0) (> iprod 0))
        (let ((lang (usb-langid dev)))
          (display "[coreusb]   mfr=\"") (display (usb-string dev iman lang))
          (display "\" product=\"") (display (usb-string dev iprod lang)) (display "\"")
          (newline)))))

(define (assq-ctx k table)
  (cond ((null? table) #f)
        ((eq? (caar table) k) (cdar table))
        (else (assq-ctx k (cdr table)))))

;; ---- enumerated-device records (for disconnect correlation) -----------------
;; record: (address hci port parent class). A disconnect on (hci,parent,port)
;; tears down the matching record: tell the class driver, tell the controller,
;; free the address. record-class lets coreusb find the class ctx to notify.
(define (rec-addr r)   (car r))
(define (rec-hci r)    (cadr r))
(define (rec-port r)   (caddr r))
(define (rec-parent r) (cadddr r))
(define (rec-class r)  (nth r 4))

;; Partition records into (matching . rest) for a disconnect on (hci,parent,port).
(define (split-matching recs hci parent port)
  (let loop ((rs recs) (hit '()) (keep '()))
    (cond ((null? rs) (cons hit keep))
          ((and (eq? (rec-hci (car rs)) hci)
                (eq? (rec-parent (car rs)) parent)
                (= (rec-port (car rs)) port))
           (loop (cdr rs) (cons (car rs) hit) keep))
          (else (loop (cdr rs) hit (cons (car rs) keep))))))

;; Notify the owning class driver + controller and return the freed addresses.
(define (teardown-records hits class-table)
  (for-each
    (lambda (r)
      (let ((cdrv (assq-ctx (rec-class r) class-table)))
        (if cdrv (send cdrv (list 'remove (rec-addr r)))))
      (send (rec-hci r) (list 'disconnect-dev (rec-addr r))))
    hits))

;; ---- the coreusb service ----------------------------------------------------
;; State: (class-table used-addrs records). class-table is an alist; used-addrs a
;; list of allocated bus addresses; records the enumerated-device list above.
(define (start-usb-service)
  (serve (list '() '() '())
    (lambda (state m)
      (let ((classes (car state)) (used (cadr state)) (recs (caddr state)) (me (self)))
        (cond
          ((eq? (car m) 'register-class)            ; (... class ctx)
           (display "[coreusb] class driver registered for class ")
           (display (cadr m)) (newline)
           (list (cons (cons (cadr m) (caddr m)) classes) used recs))

          ((eq? (car m) 'port-connected)            ; (... hci port speed)
           (start-enum me classes used recs (cadr m) 0 (caddr m) (cadddr m)))

          ((eq? (car m) 'enumerate-downstream)      ; (... hci parent port speed)
           (start-enum me classes used recs (cadr m) (caddr m) (cadddr m) (nth m 4)))

          ((eq? (car m) 'port-disconnected)         ; (... hci port)
           (let* ((sp (split-matching recs (cadr m) 0 (caddr m)))
                  (hits (car sp)) (keep (cdr sp)))
             (teardown-records hits classes)
             (list classes
                   (let loop ((a used) (h hits))
                     (if (null? h) a (loop (remove-elt (rec-addr (car h)) a) (cdr h))))
                   keep)))

          ((eq? (car m) 'disconnect-downstream)     ; (... hci parent port)
           (let* ((sp (split-matching recs (cadr m) (caddr m) (cadddr m)))
                  (hits (car sp)) (keep (cdr sp)))
             (teardown-records hits classes)
             (list classes
                   (let loop ((a used) (h hits))
                     (if (null? h) a (loop (remove-elt (rec-addr (car h)) a) (cdr h))))
                   keep)))

          ((eq? (car m) 'enum-done)                 ; (... addr hci port parent class)
           (list classes used
                 (cons (list (cadr m) (caddr m) (cadddr m) (nth m 4) (nth m 5)) recs)))

          ((eq? (car m) 'enum-failed)               ; (... addr)
           (list classes (remove-elt (cadr m) used) recs))

          (else state))))))

;; Allocate an address and spawn an enumerator for it; returns the new service
;; state (classes used recs) with the address reserved. On exhaustion, logs and
;; leaves state unchanged. `me` is the coreusb handle the enumerator reports to.
(define (start-enum me classes used recs hci parent port speed)
  (let ((addr (addr-alloc used)))
    (if (not addr)
        (begin (display "[coreusb] out of USB bus addresses") (newline)
               (list classes used recs))
        (begin
          (spawn-restricted '()
            (lambda () (enumerate hci port speed parent addr classes me)))
          ;; record bookkeeping happens on the enumerator's (enum-done) report; here
          ;; we only reserve the address so a concurrent connect cannot reuse it.
          (list classes (cons addr used) recs)))))
