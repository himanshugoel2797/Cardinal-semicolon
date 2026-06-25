;; uhci/xfer -- the synchronous control and data (interrupt/bulk) transfer
;; engines. Ported from uhci_control_transfer / uhci_data_transfer in
;; drivers/uhci/src/main.c. No ctrl_lock is needed: the host-controller CONTEXT
;; services one transfer message at a time, so it IS the serialization the C
;; lock provided. The poll loop YIELDS (sleep) between checks -- the controller
;; writes TD status to DMA independently, so we just wait for it to retire while
;; the scheduler runs other contexts.

(define MS-100 100000000)   ; 100ms in ns
(define MS-5     5000000)   ; 5ms
(define MS-200 200000000)   ; 200ms

(define (any-fatal? dma n)
  (let loop ((i 0)) (cond ((= i n) #f) ((td-fatal? dma i) #t) (else (loop (+ i 1))))))
(define (any-active? dma n)
  (let loop ((i 0)) (cond ((= i n) #f) ((td-active? dma i) #t) (else (loop (+ i 1))))))

;; Poll until `done?` (a thunk over the TD state), a fatal latch, or timeout.
(define (poll-until dma n done? timeout-ns)
  (let ((deadline (+ (uptime-ns) timeout-ns)))
    (let loop ()
      (cond ((any-fatal? dma n) 'fatal)
            ((done?) 'done)
            ((> (uptime-ns) deadline) 'timeout)
            (else (sleep 100000) (loop))))))

;; Control transfer on endpoint 0. setup is an 8-byte bytevector; data is an OUT
;; payload bytevector or #f; len is wLength. Returns (list n data) where data is
;; a fresh bytevector for an IN transfer (or #f), and n is the byte count (or -1).
(define (uhci-control iobar dma dma-phys addr speed mps setup data len)
  (let* ((ls (if (= speed USB-SPEED-LOW) 1 0))
         (mps (if (<= mps 0) 8 mps))
         (len (cond ((< len 0) 0) ((> len UHCI-DATA-MAX) UHCI-DATA-MAX) (else len)))
         (setup-phys (+ dma-phys UHCI-SETUP-OFF))
         (data-phys  (+ dma-phys UHCI-DATA-OFF))
         (td-base    (+ dma-phys UHCI-TD-OFF))
         (is-read (not (= 0 (bitwise-and (bytes-u8-ref setup 0) USB-REQ-DIR-IN)))))
    (bytes-copy-into! dma UHCI-SETUP-OFF setup 8)
    (if (and (> len 0) (not is-read) data)
        (bytes-copy-into! dma UHCI-DATA-OFF data len))
    ;; SETUP stage (8 bytes, DATA0).
    (td! dma 0 (td-status-dw ls) (td-token-dw UHCI-PID-SETUP addr 0 0 7) setup-phys)
    ;; DATA stage: max_packet chunks, toggle alternating from 1. Returns the index
    ;; of the STATUS stage (= number of SETUP+DATA TDs built).
    (let ((status-idx
            (let dl ((n 1) (toggle 1) (rem len) (bptr data-phys))
              (if (and (> rem 0) (< n (- UHCI-TD-COUNT 1)))
                  (let ((chunk (if (> rem mps) mps rem)))
                    (td! dma n (td-status-dw ls)
                         (td-token-dw (if is-read UHCI-PID-IN UHCI-PID-OUT)
                                      addr 0 toggle (- chunk 1)) bptr)
                    (dl (+ n 1) (bitwise-xor toggle 1) (- rem chunk) (+ bptr chunk)))
                  n))))
      ;; STATUS stage: opposite direction, zero-length, DATA1.
      (td! dma status-idx (td-status-dw ls)
           (td-token-dw (if is-read UHCI-PID-OUT UHCI-PID-IN) addr 0 1 #x7FF) 0)
      (let ((n (+ status-idx 1)))
        (link-chain! dma td-base n)
        (qh-arm! dma td-base)
        (let ((r (poll-until dma n (lambda () (not (td-active? dma status-idx))) MS-100)))
          (qh-idle! dma)
          (if (not (eq? r 'done))
              (list -1 #f)
              (let ((total (let sl ((i 1) (acc 0))
                             (if (>= i status-idx) acc
                                 (let ((al (td-actlen dma i)))
                                   (sl (+ i 1) (+ acc (if (= al #x7FF) 0 (+ al 1)))))))))
                (if (and is-read (> total 0))
                    (let ((c (if (> total len) len total)))
                      (list c (copy-bytes dma UHCI-DATA-OFF c)))
                    (list total #f)))))))))

;; Isochronous transfer (audio streaming). Splits `len` bytes into ceil(len/mps)
;; packets, one per frame, placed DIRECTLY in consecutive frame-list slots (each
;; iso TD links onward to the control QH) starting a couple frames ahead of the
;; HC's current frame. Iso has no handshake/retry: every TD is clocked out in its
;; frame and goes inactive, so we just wait for the last frame to pass, restore the
;; frame slots, and sum the actual lengths. `fl` is the frame list, `qh-phys` the
;; control QH phys, `itd`/`idata` the iso TD + data DMA buffers. Returns (list n
;; data) like the other engines (data is fresh bytes for IN, else #f).
(define (uhci-isoch iobar fl qh-phys itd itd-phys idata idata-phys addr ep dir-in? mps data len)
  (let* ((mps (if (<= mps 0) 8 mps))
         (len (cond ((< len 0) 0) ((> len ISO-DATA-MAX) ISO-DATA-MAX) (else len)))
         (ep (bitwise-and ep #xF))
         (pid (if dir-in? UHCI-PID-IN UHCI-PID-OUT))
         (n (let ((p (quotient (+ len (- mps 1)) mps))) (cond ((< p 1) 1) ((> p ISO-TD-COUNT) ISO-TD-COUNT) (else p))))
         (start (bitwise-and (+ (uhci-frnum iobar) 2) #x3FF)))
    (if (and (not dir-in?) data (> len 0))
        (bytes-copy-into! idata 0 data len))
    ;; build + schedule one iso TD per packet
    (let build ((i 0))
      (if (< i n)
          (let ((chunk (let ((c (- len (* i mps)))) (cond ((< c 0) 0) ((> c mps) mps) (else c)))))
            (iso-td! itd i qh-phys
                     (td-token-dw pid addr ep 0 (if (> chunk 0) (- chunk 1) #x7FF))
                     (+ idata-phys (* i mps)))
            (frame-set-iso! fl (bitwise-and (+ start i) #x3FF) (+ itd-phys (* i 16)))
            (build (+ i 1)))))
    ;; wait for every scheduled iso TD to retire (its frame to pass), bounded
    (let ((deadline (+ (uptime-ns) (* (+ n 8) 1000000))))
      (let wait ()
        (if (and (let any ((i 0)) (cond ((= i n) #f) ((iso-td-active? itd i) #t) (else (any (+ i 1)))))
                 (< (uptime-ns) deadline))
            (begin (sleep 100000) (wait)))))
    ;; restore the frame slots to the persistent control QH
    (let restore ((i 0))
      (if (< i n) (begin (frame-set-qh! fl (bitwise-and (+ start i) #x3FF) qh-phys) (restore (+ i 1)))))
    (let ((total (let sl ((i 0) (acc 0))
                   (if (>= i n) acc
                       (let ((al (iso-td-actlen itd i)))
                         (sl (+ i 1) (+ acc (if (= al #x7FF) 0 (+ al 1)))))))))
      (if (and dir-in? (> total 0))
          (list total (copy-bytes idata 0 (if (> total len) len total)))
          (list total #f)))))

;; Single-endpoint data transfer (interrupt or bulk) using the driver-tracked
;; per-(address,endpoint) data toggle in `ep-toggle` (a 128*16 byte buffer).
;; Returns (list n data) like uhci-control.
(define (uhci-data iobar dma dma-phys ep-toggle addr ls endpoint dir-in? mps data len timeout-ns)
  (let* ((mps (if (<= mps 0) 8 mps))
         (len (cond ((< len 0) 0) ((> len UHCI-DATA-MAX) UHCI-DATA-MAX) (else len)))
         (ep (bitwise-and endpoint #xF))
         (data-phys (+ dma-phys UHCI-DATA-OFF))
         (td-base   (+ dma-phys UHCI-TD-OFF))
         (tidx (+ (* (bitwise-and addr #x7F) 16) ep))
         (pid (if dir-in? UHCI-PID-IN UHCI-PID-OUT)))
    (if (and (not dir-in?) data (> len 0))
        (bytes-copy-into! dma UHCI-DATA-OFF data len))
    (let* ((start-toggle (bitwise-and (bytes-u8-ref ep-toggle tidx) 1))
           (built (let bl ((n 0) (toggle start-toggle) (rem len) (bptr data-phys))
                    (let ((chunk (if (> rem mps) mps rem)))
                      (td! dma n (td-status-dw ls)
                           (td-token-dw pid addr ep toggle (if (> chunk 0) (- chunk 1) #x7FF))
                           (if (> chunk 0) bptr 0))
                      (if (and (> (- rem chunk) 0) (< (+ n 1) UHCI-TD-COUNT))
                          (bl (+ n 1) (bitwise-xor toggle 1) (- rem chunk) (+ bptr chunk))
                          (list (+ n 1) (bitwise-xor toggle 1))))))
           (n (car built)) (next-toggle (cadr built)))
      (link-chain! dma td-base n)
      (qh-arm! dma td-base)
      (let ((r (poll-until dma n (lambda () (not (any-active? dma n))) timeout-ns)))
        (qh-idle! dma)
        (cond
          ((eq? r 'done)
           (bytes-u8-set! ep-toggle tidx next-toggle)
           (let ((total (let sl ((i 0) (acc 0))
                          (if (>= i n) acc
                              (let ((al (td-actlen dma i)))
                                (sl (+ i 1) (+ acc (if (= al #x7FF) 0 (+ al 1)))))))))
             (let ((c (if (> total len) len total)))
               (if dir-in?
                   (list c (copy-bytes dma UHCI-DATA-OFF c))
                   (list c #f)))))
          ;; an IN poll that only NAK'd within budget = no data available now.
          ((and (eq? r 'timeout) dir-in?) (list 0 (make-bytes 0)))
          (else (list -1 #f)))))))
