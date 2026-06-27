;; virtio-console: the virtio 1.0 console driver (PCI 1af4:1043), single-port.
;;
;; We deliberately do NOT negotiate VIRTIO_CONSOLE_F_MULTIPORT, so the device
;; presents the simple two-queue port0 layout: queue 0 is the receiveq
;; (device->driver input) and queue 1 is the transmitq (driver->device output).
;; Device config is cols(u16@0) rows(u16@2) -- read but otherwise unused here.
;;
;; The driver runs a small service context: send it (list 'write <string|bytes>)
;; to transmit, or (list 'subscribe <ctx>) to receive forwarded input. On bring-
;; up it transmits a one-line banner so a QEMU chardev smoke test shows output.
;;
;; TX is the interesting path; RX is intentionally minimal (drain the receiveq
;; and forward each chunk to the optional subscriber, recycling the buffer). The
;; pure half -- packing a string into a device-readable byte buffer -- is
;; factored into `console-pack` for host unit testing with no device.
(define-module virtio-console
  (export console-init console-pack)
  (import sys-mmio sys-pci driver-util virtio)
  (define lg (make-logger 'virtio-console))

;; --- pure: string/bytes -> a fresh owned byte buffer -----------------------
;; The transmit path needs the message payload as a contiguous byte buffer. A
;; caller may hand us a string or an already-packed bytes; console-pack
;; normalises either into a fresh `bytes` (and its length is bytes-length).
;; Strings are copied a char-code at a time via string-ref/char->integer, which
;; is the only string->bytes path the kernel Lisp offers. Pure and allocating
;; only -- host-testable.
(define (console-pack s)
  (if (string? s)
      (let ((n (string-length s)))
        (let ((b (make-bytes (if (= n 0) 1 n))))
          (let loop ((i 0))
            (if (>= i n)
                b
                (begin (bytes-u8-set! b i (char->integer (string-ref s i)))
                       (loop (+ i 1)))))))
      s))   ; already bytes

;; The valid byte length of a packed payload: a packed empty string is a
;; 1-byte buffer holding nothing meaningful, so length is taken from the source.
(define (console-len s)
  (if (string? s) (string-length s) (bytes-length s)))

;; --- RX ---------------------------------------------------------------------

(define NRX 8)
(define RXSLOT 128)

(define (rx-populate! rxq rxbuf notify mult)
  (let ((base (bytes-phys rxbuf)) (desc (q-desc rxq)) (avail (q-avail rxq))
        (n (if (< (q-size rxq) NRX) (q-size rxq) NRX)))
    (let loop ((i 0))
      (if (= i n)
          (begin (notify-queue! notify mult rxq) 'done)
          (begin
            (desc-set! desc i (+ base (* i RXSLOT)) RXSLOT VIRTQ-DESC-F-WRITE 0)
            (avail-push! avail (q-size rxq) i)
            (loop (+ i 1)))))))

;; Drain newly-used RX descriptors; for each, snapshot the input bytes out of
;; the recycled slot and forward them to `sub` (a context, or #f to discard),
;; then recycle the descriptor. `last` is a cell holding the consumed used-idx.
(define (rx-drain! rxq rxbuf last notify mult sub)
  (let ((used (q-used rxq)) (avail (q-avail rxq)) (qsize (q-size rxq)))
    (let loop ((li (cell-ref last)))
      (if (= li (bytes-u16-ref used 2))
          (cell-set! last li)
          (let* ((slot (modulo li qsize))
                 (id   (bytes-u32-ref used (+ 4 (* 8 slot))))
                 (ulen (bytes-u32-ref used (+ 8 (* 8 slot)))))
            (if (and (ctx? sub) (> ulen 0))
                (send sub (list 'console-rx (copy-bytes rxbuf (* id RXSLOT) ulen))))
            (avail-push! avail qsize id)
            (notify-queue! notify mult rxq)
            (loop (bitwise-and (+ li 1) #xFFFF)))))))

;; --- TX ---------------------------------------------------------------------
;; Single TX buffer, one frame in flight: copy the payload in, post descriptor 0
;; as device-READABLE, kick, wait for the device to consume it before returning
;; (so the next write never overwrites a buffer still being read).

(define TXBUF 256)
;; A paravirt console drains the transmitq in microseconds; this only bounds a
;; genuinely wedged device. Keep it generous (100 ms) -- a 1 ms timeout would
;; spuriously fire under TCG, and tx-write! must NOT return success on a timeout
;; (the avail.idx is already advanced, so the next call would post descriptor 0 a
;; second time against an entry the device never consumed -- a split-ring desync).
(define TX-TIMEOUT 100000000)

;; Transmit one buffer; single outstanding descriptor (slot 0). Returns the byte
;; count on completion, or #f on timeout so the caller drops rather than treating
;; a wedged device as a successful write.
(define (tx-write! txq txbuf notify mult payload len)
  (let ((n (if (> len TXBUF) TXBUF len))
        (before (bytes-u16-ref (q-used txq) 2)))
    (bytes-copy! txbuf 0 payload 0 n)
    (desc-set! (q-desc txq) 0 (bytes-phys txbuf) n 0 0)
    (avail-push! (q-avail txq) (q-size txq) 0)
    (notify-queue! notify mult txq)
    (if (wait-until-spin (lambda () (not (= (bytes-u16-ref (q-used txq) 2) before)))
                         TX-TIMEOUT 500000)
        n
        #f)))

;; --- bring-up ---------------------------------------------------------------

(define VIRTIO-CONSOLE-VID #x1af4)
(define VIRTIO-CONSOLE-DID #x1043)
(define BANNER "Cardinal; virtio-console up\n")

;; console-init brings the device up and spawns the console service, returning
;; its handle (or #f). Runs at init time (NOT under the scheduler): it must not
;; block, so it spawns the service and returns. The banner is transmitted from
;; INSIDE the service context (its first self-sent message) so the blocking
;; tx wait happens under the scheduler, never in init.
(define (console-init ecam)
  (if (not ecam)
      (begin (lg "no device present") #f)
      ;; Single-port: negotiate only VERSION_1 (no MULTIPORT).
      (let ((dev (virtio-bringup ecam 0 (arithmetic-shift 1 VIRTIO-F-VERSION-1-BIT))))
        (if (not dev)
            (begin (lg "device rejected FEATURES_OK") #f)
            (let ((common (nth dev 0)) (devcfg (nth dev 1))
                  (notify (nth dev 2)) (mult (nth dev 3)))
              (let ((rxq (virtio-setup-queue common 0))
                    (txq (virtio-setup-queue common 1)))
                (if (or (not rxq) (not txq))
                    (begin (lg "queue setup failed") #f)
                    (begin
                      (virtio-status-set! common VIRTIO-STATUS-DRIVER-OK)
                      (let ((cols (bytes-u16-ref devcfg 0))
                            (rows (bytes-u16-ref devcfg 2)))
                        (lg "up: cols=" cols " rows=" rows))
                      (let ((rxbuf (dma-alloc (* NRX RXSLOT)))
                            (txbuf (dma-alloc TXBUF))
                            (last  (make-cell 0))
                            (sub   (make-cell #f)))
                        (rx-populate! rxq rxbuf notify mult)
                        ;; The service: 'write transmits, 'subscribe registers an
                        ;; RX sink, 'poll-rx drains the receiveq (a subscriber may
                        ;; send it on a timer; RX is otherwise quiescent here).
                        (let ((svc
                               (serve 'console
                                 (lambda (state m)
                                   (cond
                                     ((not (pair? m)) state)
                                     ((eq? (car m) 'write)
                                      (let ((p (console-pack (cadr m))))
                                        (tx-write! txq txbuf notify mult p
                                                   (console-len (cadr m))))
                                      state)
                                     ((eq? (car m) 'subscribe)
                                      (if (ctx? (cadr m)) (cell-set! sub (cadr m)))
                                      state)
                                     ((eq? (car m) 'poll-rx)
                                      (rx-drain! rxq rxbuf last notify mult
                                                 (cell-ref sub))
                                      state)
                                     (else state))))))
                          ;; Banner: queued to the service so the blocking tx runs
                          ;; under the scheduler, not in init.
                          (send svc (list 'write BANNER))
                          svc)))))))))))
