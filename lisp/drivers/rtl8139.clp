;; rtl8139: the Realtek RTL8139 NIC, in Cardinal Lisp.
;;
;; The C driver this supersedes (drivers/rtl8139/) brought the device to RX/TX
;; ENABLED but its RX path was a STUB -- the ISR only acked ROK/TOK and never
;; pulled a frame off the ring, and IMR was left 0. So this is not a line-by-line
;; port: the standard 8139 receive-ring extraction (the CAPR/CBR-driven circular
;; buffer with the 4-byte rx-header + WRAP slop) is implemented here from scratch.
;;
;; The 8139 is a LEGACY device: registers live in a memory BAR (BAR1), it DMAs
;; 32-bit physical addresses only (hence dma-alloc-32, MANDATORY <4GB), and it has
;; no MSI -- only a legacy INTx line. Rather than wire a PCI-INTx -> Lisp-irq
;; bridge, RX is POLLED from a context that (sleep)s between drains; the sleep
;; YIELDS, so the core runs other contexts between polls. (A future PCI-INTx ->
;; irq-wait bridge could replace the poll with a parked wait; the structure here
;; -- a count/loop in a spawned context -- already matches that shape.)
;;
;; Like virtio-net, this driver is pure transport: it registers with the
;; corenetwork service (handing it the MAC + a TX context) and forwards every
;; received frame to it; all ethernet/ARP/IP framing lives in the service.
;;
;; Capabilities: sys-mmio (mmio-map + dma-alloc-32), sys-pci (pci-find +
;; pci-assign-bars), and the generic driver-util helpers. NO sys-io (registers are
;; MMIO via BAR1), NO MSI (legacy INTx), NO sys-irq (polling).
(define-module rtl8139
  (export rtl8139-init rx-parse-one tx-fill! rx-extract!)
  (import sys-mmio sys-pci driver-util)

;; --- register map (MMIO BAR1, mapped over a #x100 window) --------------------

(define IDR0          #x00)   ; 6-byte MAC
(define TX-CMD        #x37)   ; the command register (u8)
(define CAPR          #x38)   ; u16: current address of packet read (the read ptr)
(define CBR           #x3A)   ; u16: current buffer address (the device write ptr)
(define IMR           #x3C)   ; u16: interrupt mask (left 0 -- we poll)
(define ISR-REG       #x3E)   ; u16: interrupt status
(define TX-CFG        #x40)   ; u32: transmit configuration
(define RCR           #x44)   ; u32: receive configuration
(define CONFIG1       #x52)   ; u8
(define MEDIA-STATUS  #x58)   ; u8

(define (TX-STS i)  (+ #x10 (* i 4)))   ; u32: per-descriptor transmit status
(define (TX-ADDR i) (+ #x20 (* i 4)))   ; u32: per-descriptor transmit-buffer phys
(define RBSTART       #x30)             ; u32: receive-buffer-start phys

;; CMD (TX-CMD) bits.
(define CMD-RST   #x10)
(define CMD-RXEN  #x08)
(define CMD-TXEN  #x04)
(define CMD-BUFE  #x01)   ; receive buffer EMPTY (set => no data to read)

;; TX-STS bits.
(define TX-OWN (arithmetic-shift 1 13))
(define TX-TOK (arithmetic-shift 1 15))

;; RCR: accept PHYS-match | MULTICAST | BROADCAST (#x0F low bits = ALL|PHYS|MCAST
;; |BCAST), WRAP (bit 7 -- let an overrunning frame run past the ring end into the
;; slop instead of wrapping mid-frame), and a 64K rx buffer (3<<11).
(define RCR-VALUE
  (bitwise-or #x0F (arithmetic-shift 1 7) (arithmetic-shift 3 11)))

;; Geometry. The 64K ring + the 16-byte CAPR offset bias + a frame's worth of WRAP
;; overrun slop. The read offset wraps modulo 64K (the configured buffer length).
(define RX-RING-LEN 65536)
(define RX-BUF-SIZE (+ 65536 16 1500))
(define TX-SLOT-SIZE 2048)
(define NTX 4)

;; RX poll interval: 1ms. (sleep) YIELDS, so the core runs other contexts between
;; drains. A future PCI-INTx -> irq-wait bridge could replace this with a parked
;; wait woken by the device's receive interrupt.
(define RX-POLL-NS 1000000)

;; --- RX: a pure parser + a poll-with-sleep extraction loop -------------------
;; The 8139 rx ring is a flat circular byte buffer. Each received frame is
;; preceded by a 4-byte header: status (u16) then length (u16, INCLUDING the
;; 4-byte ethernet CRC). The frame bytes follow; the next header is dword-aligned.

;; Pure: parse the rx-header at `off` -> (list frame-off frame-len next-off).
;; frame-len strips the trailing CRC; next-off is the dword-rounded start of the
;; following header, wrapped modulo the 64K ring.
(define (rx-parse-one rxbuf off)
  (let* ((length    (bytes-u16-ref rxbuf (+ off 2)))    ; includes the 4-byte CRC
         (frame-off (+ off 4))
         (frame-len (- length 4))
         (next-off  (modulo (bitwise-and (+ off length 4 3) (bitwise-not 3))
                            RX-RING-LEN)))
    (list frame-off frame-len next-off)))

;; A header's status word: bit0 = ROK (receive OK). We forward only ROK frames of
;; at least a full ethernet header.
(define (rx-status-ok? status) (= 1 (bitwise-and status 1)))

;; Drain every frame currently in the ring, calling (handler frame-off frame-len)
;; for each good one, advancing the read-offset cell and writing CAPR back so the
;; device may reuse the space. BUFE clear (CMD bit0 = 0) means data is present.
(define (rx-extract! regs rxbuf off-cell handler)
  (let loop ()
    (if (= 0 (bitwise-and (bytes-u8-ref regs TX-CMD) CMD-BUFE))
        (let* ((off    (cell-ref off-cell))
               (status (bytes-u16-ref rxbuf off))
               (p      (rx-parse-one rxbuf off))
               (foff   (nth p 0))
               (flen   (nth p 1))
               (noff   (nth p 2)))
          (if (and (rx-status-ok? status) (>= flen 14))
              (handler foff flen))
          (cell-set! off-cell noff)
          ;; CAPR trails the read pointer by 16 (the device's quirk); mask to u16.
          (bytes-u16-set! regs CAPR (bitwise-and (- noff 16) #xFFFF))
          (loop))
        'drained)))

;; --- TX: a 4-slot round-robin -----------------------------------------------
;; Pure-ish core (buffers + the free-slot cell are args, so it is testable without
;; hardware): copy the frame into the next slot and kick the device by writing its
;; length into TX-STS (which clears OWN and starts the DMA). Advance the cell.

(define (tx-fill! regs txbufs free-cell frame len)
  (if (or (> len TX-SLOT-SIZE) (<= len 0))
      #f
      (let ((i (cell-ref free-cell)))
        ;; Wait (bounded) for any prior transmit on this slot to finish: the
        ;; device sets OWN|TOK when done. wait-until yields between polls.
        (wait-until
          (lambda ()
            (let ((sts (bytes-u32-ref regs (TX-STS i))))
              (= 0 (bitwise-and sts #xFFF))))   ; size field clear => slot idle
          100000000)                            ; 100ms
        (bytes-copy-into! (nth txbufs i) 0 frame len)
        ;; Writing the size (low 13 bits; OWN auto-clears) starts the transmit.
        (bytes-u32-set! regs (TX-STS i) (bitwise-and len #xFFF))
        (cell-set! free-cell (modulo (+ i 1) NTX))
        'sent)))

;; --- bring-up ----------------------------------------------------------------

(define RTL-VID #x10EC)
(define RTL-DID #x8139)

(define (read-mac regs)
  (list (bytes-u8-ref regs (+ IDR0 0)) (bytes-u8-ref regs (+ IDR0 1))
        (bytes-u8-ref regs (+ IDR0 2)) (bytes-u8-ref regs (+ IDR0 3))
        (bytes-u8-ref regs (+ IDR0 4)) (bytes-u8-ref regs (+ IDR0 5))))

;; Allocate the four TX slots as a list of <4GB DMA buffers, or #f if any is out
;; of (or above) 32-bit physical memory.
(define (alloc-tx-bufs)
  (let loop ((i 0) (acc '()))
    (if (= i NTX)
        (reverse acc)
        (let ((b (dma-alloc-32 TX-SLOT-SIZE)))
          (if (or (not b) (>= (bytes-phys b) #x100000000))
              #f
              (loop (+ i 1) (cons b acc)))))))

;; rtl8139-init takes the corenetwork service handle. It runs INSIDE a spawned
;; context (so wait-until/sleep yield); discovery + config-space setup are
;; synchronous, the reset settle uses wait-until (which yields). On any failure it
;; logs and returns #f. On success it spawns a TX context and an RX poll context,
;; registers with the network stack, and returns 'ok.
;; `dev-ecam` is supplied by init (one per enumerated NIC); see virtio-net-init.
(define (rtl8139-init net dev-ecam)
  (let ((ecam dev-ecam))
    (if (not ecam)
        (begin (display "[rtl8139] no device present") (newline) #f)
        (let ((cfg (mmio-map ecam #x1000)))
          (pci-enable-mem-bus-master! cfg)
          ;; BAR1 holds the MMIO registers. Firmware usually assigns it; if not,
          ;; place the BARs ourselves and re-read.
          (let* ((b0 (bar-base cfg 1))
                 (base (if (= b0 0) (begin (pci-assign-bars ecam) (bar-base cfg 1)) b0)))
            (if (= base 0)
                (begin (display "[rtl8139] no BAR1 register window") (newline) #f)
                (let ((regs (mmio-map base #x100)))
                  ;; Power on, then software-reset and wait for the RST bit to
                  ;; self-clear (bounded -- a wedged/absent NIC must not hang boot).
                  (bytes-u8-set! regs CONFIG1 0)
                  (bytes-u8-set! regs TX-CMD CMD-RST)
                  (if (not (wait-until
                             (lambda () (= 0 (bitwise-and (bytes-u8-ref regs TX-CMD) CMD-RST)))
                             100000000))
                      (begin (display "[rtl8139] reset timed out") (newline) #f)
                      (let ((mac    (read-mac regs))
                            (rxbuf  (dma-alloc-32 RX-BUF-SIZE))
                            (txbufs (alloc-tx-bufs)))
                        (if (or (not rxbuf) (>= (bytes-phys rxbuf) #x100000000) (not txbufs))
                            (begin (display "[rtl8139] DMA buffer alloc failed (need <4GB)") (newline) #f)
                            (begin
                              ;; Program the ring + TX-slot physical addresses.
                              (bytes-u32-set! regs RBSTART (bytes-phys rxbuf))
                              (let loop ((i 0))
                                (if (< i NTX)
                                    (begin
                                      (bytes-u32-set! regs (TX-ADDR i) (bytes-phys (nth txbufs i)))
                                      (loop (+ i 1)))))
                              ;; Configure RX, then TX (preserve the IFG bits in
                              ;; TX-CFG, set max DMA burst 1024 = 6<<8).
                              (bytes-u32-set! regs RCR RCR-VALUE)
                              (bytes-u32-set! regs TX-CFG
                                (bitwise-or (bitwise-and (bytes-u32-ref regs TX-CFG) #x03080000)
                                            (arithmetic-shift 6 8)))
                              ;; Enable RX+TX LAST. IMR stays 0 -- we poll.
                              (bytes-u8-set! regs TX-CMD (bitwise-or CMD-RXEN CMD-TXEN))
                              (let ((off-cell (make-cell 0))
                                    (free-cell (make-cell 0)))
                                ;; TX context: the network service sends (tx frame
                                ;; len); copy it into the next slot and start it.
                                (let ((tx-ctx
                                        (spawn-restricted '() (lambda ()
                                          (let loop ()
                                            (let ((m (recv)))
                                              (if (eq? (car m) 'tx)
                                                  (begin
                                                    (tx-fill! regs txbufs free-cell (cadr m) (caddr m))
                                                    (if (> (length m) 3) (send (nth m 3) (list 'tx-done)))))
                                              (loop)))))))
                                  ;; RX context: drain the ring, forward each frame
                                  ;; (snapshotted out of the recycled ring) to the
                                  ;; service, then sleep (yielding) and poll again.
                                  (spawn-restricted '() (lambda ()
                                    (let loop ()
                                      (rx-extract! regs rxbuf off-cell
                                        (lambda (foff flen)
                                          (send net (list 'rx (copy-bytes rxbuf foff flen) flen))))
                                      (sleep RX-POLL-NS)
                                      (loop))))
                                  (send net (list 'register-nic mac tx-ctx))
                                  (display "[rtl8139] registered with network stack") (newline)
                                  'ok)))))))))))))) ; last ) closes (define-module rtl8139 ...)
