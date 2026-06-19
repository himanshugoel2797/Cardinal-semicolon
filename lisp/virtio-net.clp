;; virtio-net: the first device driver written in Cardinal Lisp.
;;
;; Loaded from the initrd at boot (single-core, into the shared env) as pure
;; definitions; SysLisp calls (virtio-net-init) later on the BSP. Built on the
;; driver substrate: pci-find, mmio-map, dma-alloc, the volatile byte accessors,
;; the bitwise/bitfield primitives, and the MSI/ISR-wake bridge (pci-setup-msi,
;; net-count, net-wait).
;;
;;   N2: discover, negotiate features, set up the RX/TX virtqueues, DRIVER_OK, MAC.
;;   N3: MSI-X -> ISR -> a Lisp RX context draining the used ring.
;;   N4: TX an ARP request and recognise the reply -> both directions end-to-end.

;; --- small utilities --------------------------------------------------------

;; The driver is a module. It imports exactly the capabilities it needs --
;; sys-mmio (mmio-map/dma-alloc) and sys-pci (pci-find/pci-setup-msi + the MSI
;; wake bridge net-count/net-wait) -- plus the generic driver-util helpers (nth
;; and the mutable word cell make-cell/cell-ref/cell-set!). It exports just the
;; entry point virtio-net-init. The capability prims stay private to this module;
;; nothing reaches them through `virtio-net`. (Bodies stay at column 0: this is a
;; pure wrapper over the existing definitions.)
(define-module virtio-net
  (export virtio-net-init)
  (import sys-mmio sys-pci driver-util)

;; --- PCI config space + virtio capability walk ------------------------------

(define PCI-COMMAND  #x04)   ; u16: bit1 = memory space, bit2 = bus master
(define PCI-CAP-PTR  #x34)   ; u8 : offset of the first capability
(define PCI-CAP-VNDR #x09)   ; vendor-specific capability id (virtio uses this)

(define VIRTIO-CFG-COMMON 1)
(define VIRTIO-CFG-NOTIFY 2)
(define VIRTIO-CFG-DEVICE 4)

;; Find the virtio capability of a given cfg-type; returns its config offset or #f.
(define (find-virtio-cap cfg cfg-type)
  (let loop ((ptr (bytes-u8-ref cfg PCI-CAP-PTR)))
    (if (= ptr 0)
        #f
        (if (and (= (bytes-u8-ref cfg ptr) PCI-CAP-VNDR)
                 (= (bytes-u8-ref cfg (+ ptr 3)) cfg-type))
            ptr
            (loop (bytes-u8-ref cfg (+ ptr 1)))))))

;; Resolve a BAR's base physical address (handles 64-bit memory BARs).
(define (bar-base cfg bar-idx)
  (let* ((off (+ #x10 (* bar-idx 4)))
         (lo  (bytes-u32-ref cfg off)))
    (if (= (bit-extract lo 1 2) 2)
        (+ (bitwise-and lo #xFFFFFFF0)
           (arithmetic-shift (bytes-u32-ref cfg (+ off 4)) 32))
        (bitwise-and lo #xFFFFFFF0))))

;; Map the BAR window a capability points at, returning a byte region over it.
(define (map-cap cfg cap-off)
  (let ((bar (bytes-u8-ref cfg (+ cap-off 4)))
        (off (bytes-u32-ref cfg (+ cap-off 8)))
        (len (bytes-u32-ref cfg (+ cap-off 12))))
    (mmio-map (+ (bar-base cfg bar) off) len)))

;; --- virtio common-config registers (offsets within the COMMON region) ------

(define VIRTIO-DEVICE-FEATURE-SELECT 0)
(define VIRTIO-DEVICE-FEATURE        4)
(define VIRTIO-DRIVER-FEATURE-SELECT 8)
(define VIRTIO-DRIVER-FEATURE       12)
(define VIRTIO-MSIX-CONFIG          16)   ; u16
(define VIRTIO-DEVICE-STATUS        20)   ; u8
(define VIRTIO-QUEUE-SELECT         22)   ; u16
(define VIRTIO-QUEUE-SIZE           24)   ; u16
(define VIRTIO-QUEUE-MSIX           26)   ; u16
(define VIRTIO-QUEUE-ENABLE         28)   ; u16
(define VIRTIO-QUEUE-NOTIFY-OFF     30)   ; u16
(define VIRTIO-QUEUE-DESC           32)   ; u64
(define VIRTIO-QUEUE-DRIVER         40)   ; u64
(define VIRTIO-QUEUE-DEVICE         48)   ; u64

(define VIRTIO-STATUS-ACK          1)
(define VIRTIO-STATUS-DRIVER       2)
(define VIRTIO-STATUS-DRIVER-OK    4)
(define VIRTIO-STATUS-FEATURES-OK  8)

(define (virtio-status-set! common bits)
  (bytes-u8-set! common VIRTIO-DEVICE-STATUS
                 (bitwise-or (bytes-u8-ref common VIRTIO-DEVICE-STATUS) bits)))

(define (virtio-features-offered common select)
  (bytes-u32-set! common VIRTIO-DEVICE-FEATURE-SELECT select)
  (bytes-u32-ref common VIRTIO-DEVICE-FEATURE))

(define (virtio-features-accept! common select val)
  (bytes-u32-set! common VIRTIO-DRIVER-FEATURE-SELECT select)
  (bytes-u32-set! common VIRTIO-DRIVER-FEATURE val))

;; --- split virtqueue ---------------------------------------------------------
;; Layouts: descriptor = addr(u64) len(u32) flags(u16) next(u16) [16 bytes].
;; avail   = flags(u16) idx(u16) ring[size](u16). used = flags(u16) idx(u16)
;; ring[size]{id(u32) len(u32)}.

(define VIRTQ-DESC-F-NEXT  1)
(define VIRTQ-DESC-F-WRITE 2)

(define (desc-set! desc i addr len flags next)
  (let ((o (* i 16)))
    (bytes-u64-set! desc o addr)
    (bytes-u32-set! desc (+ o 8) len)
    (bytes-u16-set! desc (+ o 12) flags)
    (bytes-u16-set! desc (+ o 14) next)))

;; Publish descriptor d into the avail ring and bump the free-running 16-bit idx.
(define (avail-push! avail qsize d)
  (let ((idx (bytes-u16-ref avail 2)))
    (bytes-u16-set! avail (+ 4 (* 2 (modulo idx qsize))) d)
    (bytes-u16-set! avail 2 (bitwise-and (+ idx 1) #xFFFF))))

;; A queue record: (size desc avail used notify-off). Accessors by position.
(define (q-size q)  (nth q 0))
(define (q-desc q)  (nth q 1))
(define (q-avail q) (nth q 2))
(define (q-used q)  (nth q 3))
(define (q-noff q)  (nth q 4))

;; Select queue qidx, allocate its rings, hand the device their physical
;; addresses, point its MSI-X vector at table entry 0, enable it.
(define (virtio-setup-queue common qidx)
  (bytes-u16-set! common VIRTIO-QUEUE-SELECT qidx)
  (let ((qsize (bytes-u16-ref common VIRTIO-QUEUE-SIZE)))
    (if (= qsize 0)
        #f
        (let ((desc  (dma-alloc (* qsize 16)))
              (avail (dma-alloc (+ 6 (* 2 qsize))))
              (used  (dma-alloc (+ 6 (* 8 qsize))))
              (noff  (bytes-u16-ref common VIRTIO-QUEUE-NOTIFY-OFF)))
          (bytes-u64-set! common VIRTIO-QUEUE-DESC   (bytes-phys desc))
          (bytes-u64-set! common VIRTIO-QUEUE-DRIVER (bytes-phys avail))
          (bytes-u64-set! common VIRTIO-QUEUE-DEVICE (bytes-phys used))
          (bytes-u16-set! common VIRTIO-QUEUE-MSIX 0)     ; interrupts -> MSI-X entry 0
          (bytes-u16-set! common VIRTIO-QUEUE-ENABLE 1)
          (list qsize desc avail used noff)))))

;; Notify the device that queue qidx has new avail entries.
(define (notify-queue! notify mult q)
  (bytes-u16-set! notify (* (q-noff q) mult) 0))   ; value is ignored by the device

;; --- RX -------------------------------------------------------------------
;; One contiguous RX buffer holds NRX slots of RXSLOT bytes; descriptor i points
;; at slot i, so the completed frame for descriptor id is at offset id*RXSLOT.

(define NRX 16)
(define RXSLOT 2048)         ; 12-byte virtio-net header + up to a 1514 frame
(define VNET-HDR 12)         ; virtio_net_hdr is 12 bytes in virtio 1.0 (num_buffers present)

;; Fill the RX descriptors (device-writable) and make them all available.
(define (rx-populate! rxq rxbuf notify mult)
  (let ((base (bytes-phys rxbuf)) (desc (q-desc rxq)) (avail (q-avail rxq))
        (n (if (< (q-size rxq) NRX) (q-size rxq) NRX)))   ; never exceed the ring
    (let loop ((i 0))
      (if (= i n)
          (begin (notify-queue! notify mult rxq) 'done)
          (begin
            (desc-set! desc i (+ base (* i RXSLOT)) RXSLOT VIRTQ-DESC-F-WRITE 0)
            (avail-push! avail (q-size rxq) i)
            (loop (+ i 1)))))))

;; Drain newly-used RX descriptors, calling (handler slot-offset frame-len) for
;; each received frame, then recycle the descriptor back into the avail ring.
(define (rx-drain! rxq rxbuf last notify mult handler)
  (let ((used (q-used rxq)) (avail (q-avail rxq)) (qsize (q-size rxq)))
    (let loop ((li (cell-ref last)))
      (if (= li (bytes-u16-ref used 2))
          (cell-set! last li)
          (let* ((slot (modulo li qsize))
                 (id   (bytes-u32-ref used (+ 4 (* 8 slot))))
                 (ulen (bytes-u32-ref used (+ 8 (* 8 slot)))))
            (handler (+ (* id RXSLOT) VNET-HDR) (- ulen VNET-HDR))  ; FRAME offset
            (avail-push! avail qsize id)          ; recycle the buffer
            (notify-queue! notify mult rxq)
            (loop (bitwise-and (+ li 1) #xFFFF)))))))

;; --- TX -------------------------------------------------------------------
;; A single TX buffer: [VNET-HDR zero bytes][ethernet frame]. Post on descriptor 0.

(define (tx-frame! txq txbuf notify mult frame-len)
  (let loop ((k 0)) (if (< k VNET-HDR) (begin (bytes-u8-set! txbuf k 0) (loop (+ k 1))) 'z))
  (desc-set! (q-desc txq) 0 (bytes-phys txbuf) (+ VNET-HDR frame-len) 0 0)
  (avail-push! (q-avail txq) (q-size txq) 0)
  (notify-queue! notify mult txq))

;; --- Ethernet + ARP (network byte order, written byte-wise) -----------------

;; Write a big-endian u16 as two bytes at off.
(define (put-be16! b off v)
  (bytes-u8-set! b off       (bit-extract v 8 8))
  (bytes-u8-set! b (+ off 1) (bit-extract v 0 8)))

;; Copy a 6-byte MAC (a list of bytes) into b at off.
(define (put-mac! b off mac)
  (let loop ((i 0) (m mac))
    (if (null? m) 'done (begin (bytes-u8-set! b (+ off i) (car m)) (loop (+ i 1) (cdr m))))))

;; Build an ARP request (who-has target-ip) into txbuf after the VNET header.
;; our-mac is a list of 6 bytes; ips are lists of 4 bytes. Returns the frame len.
(define (build-arp-request! txbuf our-mac our-ip target-ip)
  (let ((o VNET-HDR))               ; ethernet frame starts after the virtio header
    (put-mac! txbuf (+ o 0) (list #xFF #xFF #xFF #xFF #xFF #xFF))  ; dst = broadcast
    (put-mac! txbuf (+ o 6) our-mac)                               ; src
    (put-be16! txbuf (+ o 12) #x0806)                              ; ethertype = ARP
    (put-be16! txbuf (+ o 14) #x0001)                              ; htype = ethernet
    (put-be16! txbuf (+ o 16) #x0800)                              ; ptype = IPv4
    (bytes-u8-set! txbuf (+ o 18) 6)                               ; hlen
    (bytes-u8-set! txbuf (+ o 19) 4)                               ; plen
    (put-be16! txbuf (+ o 20) #x0001)                              ; oper = request
    (put-mac! txbuf (+ o 22) our-mac)                              ; sender hw
    (put-mac! txbuf (+ o 28) our-ip)                               ; sender proto (4)
    (put-mac! txbuf (+ o 32) (list 0 0 0 0 0 0))                   ; target hw = 0
    (put-mac! txbuf (+ o 38) target-ip)                            ; target proto (4)
    42))                                                          ; 14 eth + 28 arp

;; If the frame at rxbuf+off is an ARP reply, return the sender MAC (6 bytes),
;; else #f. Ethertype at +12, ARP oper at +20.
(define (arp-reply-sender rxbuf off len)
  (if (and (>= len 42)                                 ; 14 eth + 28 arp minimum
           (= (bytes-u8-ref rxbuf (+ off 12)) #x08)
           (= (bytes-u8-ref rxbuf (+ off 13)) #x06)
           (= (bytes-u8-ref rxbuf (+ off 21)) #x02))   ; oper low byte = 2 (reply)
      (list (bytes-u8-ref rxbuf (+ off 22)) (bytes-u8-ref rxbuf (+ off 23))
            (bytes-u8-ref rxbuf (+ off 24)) (bytes-u8-ref rxbuf (+ off 25))
            (bytes-u8-ref rxbuf (+ off 26)) (bytes-u8-ref rxbuf (+ off 27)))
      #f))

;; --- bring-up ---------------------------------------------------------------

(define VIRTIO-NET-VID #x1af4)
(define VIRTIO-NET-DID #x1041)
(define VIRTIO-NET-F-MAC-BIT 5)
(define VIRTIO-F-VERSION-1-BIT 0)

(define (virtio-net-read-mac devcfg)
  (list (bytes-u8-ref devcfg 0) (bytes-u8-ref devcfg 1) (bytes-u8-ref devcfg 2)
        (bytes-u8-ref devcfg 3) (bytes-u8-ref devcfg 4) (bytes-u8-ref devcfg 5)))

(define (virtio-net-init)
  (let ((ecam (pci-find VIRTIO-NET-VID VIRTIO-NET-DID)))
    (if (not ecam)
        (begin (display "[virtio-net] no device present") (newline) #f)
        (let ((cfg (mmio-map ecam #x1000)))
          (bytes-u16-set! cfg PCI-COMMAND (bitwise-or (bytes-u16-ref cfg PCI-COMMAND) #x6))
          (let* ((common  (map-cap cfg (find-virtio-cap cfg VIRTIO-CFG-COMMON)))
                 (devcfg  (map-cap cfg (find-virtio-cap cfg VIRTIO-CFG-DEVICE)))
                 (ncap    (find-virtio-cap cfg VIRTIO-CFG-NOTIFY))
                 (notify  (map-cap cfg ncap))
                 (mult    (bytes-u32-ref cfg (+ ncap 16))))   ; notify_off_multiplier
            (bytes-u8-set! common VIRTIO-DEVICE-STATUS 0)
            (virtio-status-set! common VIRTIO-STATUS-ACK)
            (virtio-status-set! common VIRTIO-STATUS-DRIVER)
            (let ((f-lo (virtio-features-offered common 0))
                  (f-hi (virtio-features-offered common 1)))
              (virtio-features-accept! common 0
                (bitwise-and f-lo (arithmetic-shift 1 VIRTIO-NET-F-MAC-BIT)))
              (virtio-features-accept! common 1
                (bitwise-and f-hi (arithmetic-shift 1 VIRTIO-F-VERSION-1-BIT))))
            (virtio-status-set! common VIRTIO-STATUS-FEATURES-OK)
            (if (= 0 (bitwise-and (bytes-u8-ref common VIRTIO-DEVICE-STATUS)
                                  VIRTIO-STATUS-FEATURES-OK))
                (begin (display "[virtio-net] device rejected FEATURES_OK") (newline) #f)
                ;; Set up the queues first -- virtio-setup-queue programs each
                ;; queue_msix_vector -- then msix_config, and only THEN enable the
                ;; MSI-X capability (pci-setup-msi): the spec wants virtio's vector
                ;; registers configured before the cap is enabled.
                (let ((rxq (virtio-setup-queue common 0))
                      (txq (virtio-setup-queue common 1))
                      (mac (virtio-net-read-mac devcfg)))
                  (bytes-u16-set! common VIRTIO-MSIX-CONFIG 0)    ; config events -> entry 0
                  (let ((vec (pci-setup-msi ecam)))
                    (if (not vec)
                        (begin (display "[virtio-net] MSI-X setup failed") (newline) #f)
                        (begin
                          (virtio-status-set! common VIRTIO-STATUS-DRIVER-OK)
                          (display "[virtio-net] up: mac=") (display mac)
                          (display " irq-vec=") (display vec) (newline)
                          ;; RX buffers + a spawned RX context, then an ARP probe.
                          (let ((rxbuf (dma-alloc (* NRX RXSLOT)))
                                (txbuf (dma-alloc RXSLOT))
                                (last  (make-cell 0))
                                (our-ip (list 10 0 2 15))      ; slirp guest
                                (gw-ip  (list 10 0 2 2)))       ; slirp gateway
                            (rx-populate! rxq rxbuf notify mult)
                            (spawn (lambda ()
                              (let loop ((seen (net-count)))
                                (rx-drain! rxq rxbuf last notify mult
                                  (lambda (off len)
                                    (let ((sender (arp-reply-sender rxbuf off len)))
                                      (if sender
                                          (begin (display "[virtio-net] RX ARP reply: gateway mac=")
                                                 (display sender) (newline))
                                          (begin (display "[virtio-net] RX frame, len=")
                                                 (display len) (newline))))))
                                (if (> (net-count) seen)
                                    (loop (net-count))
                                    (begin (net-wait seen) (loop (net-count)))))))
                            (let ((flen (build-arp-request! txbuf mac our-ip gw-ip)))
                              (tx-frame! txq txbuf notify mult flen))
                            (display "[virtio-net] sent ARP who-has 10.0.2.2") (newline)
                            'ok)))))))))))) ; last ) closes (define-module virtio-net ...)
