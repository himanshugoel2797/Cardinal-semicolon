;; virtio-net: the first device driver written in Cardinal Lisp.
;;
;; Loaded from the initrd at boot (single-core, into the shared env) as pure
;; definitions; SysLisp calls (virtio-net-init) later on the BSP. Built on the
;; driver substrate: pci-find, mmio-map, dma-alloc, the volatile byte accessors,
;; and the bitwise/bitfield primitives.
;;
;; Stage N2: discover the modern (1.0) virtio-net device, walk its PCI
;; capabilities, negotiate features, set up the RX/TX virtqueues, reach DRIVER_OK,
;; and read the MAC. RX via MSI-X (N3) and TX + networking (N4) build on this.

;; --- PCI config space + virtio capability walk ------------------------------

;; PCI config offsets (in the ECAM region).
(define PCI-COMMAND  #x04)   ; u16: bit1 = memory space, bit2 = bus master
(define PCI-CAP-PTR  #x34)   ; u8 : offset of the first capability
(define PCI-CAP-VNDR #x09)   ; vendor-specific capability id (virtio uses this)

;; A virtio_pci_cap, relative to its capability offset:
;;   +0 cap_vndr(u8) +1 cap_next(u8) +2 cap_len(u8) +3 cfg_type(u8)
;;   +4 bar(u8) +5..7 pad +8 offset(u32) +12 length(u32)
(define VIRTIO-CFG-COMMON 1)
(define VIRTIO-CFG-NOTIFY 2)
(define VIRTIO-CFG-ISR    3)
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
    (if (= (bit-extract lo 1 2) 2)                 ; type bits 2:1 == 10b => 64-bit
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

(define VIRTIO-DEVICE-FEATURE-SELECT 0)   ; u32
(define VIRTIO-DEVICE-FEATURE        4)   ; u32 (ro)
(define VIRTIO-DRIVER-FEATURE-SELECT 8)   ; u32
(define VIRTIO-DRIVER-FEATURE       12)   ; u32
(define VIRTIO-NUM-QUEUES           18)   ; u16 (ro)
(define VIRTIO-DEVICE-STATUS        20)   ; u8
(define VIRTIO-QUEUE-SELECT         22)   ; u16
(define VIRTIO-QUEUE-SIZE           24)   ; u16
(define VIRTIO-QUEUE-ENABLE         28)   ; u16
(define VIRTIO-QUEUE-DESC           32)   ; u64 (descriptor table phys)
(define VIRTIO-QUEUE-DRIVER         40)   ; u64 (avail ring phys)
(define VIRTIO-QUEUE-DEVICE         48)   ; u64 (used ring phys)

;; device_status bits
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

;; Set up one split virtqueue: allocate its descriptor table / avail ring / used
;; ring as DMA buffers, hand their physical addresses to the device, and enable
;; it. Returns (size desc avail used), or #f if the queue is unavailable.
(define (virtio-setup-queue common qidx)
  (bytes-u16-set! common VIRTIO-QUEUE-SELECT qidx)
  (let ((qsize (bytes-u16-ref common VIRTIO-QUEUE-SIZE)))
    (if (= qsize 0)
        #f
        (let ((desc  (dma-alloc (* qsize 16)))
              (avail (dma-alloc (+ 6 (* 2 qsize))))
              (used  (dma-alloc (+ 6 (* 8 qsize)))))
          (bytes-u64-set! common VIRTIO-QUEUE-DESC   (bytes-phys desc))
          (bytes-u64-set! common VIRTIO-QUEUE-DRIVER (bytes-phys avail))
          (bytes-u64-set! common VIRTIO-QUEUE-DEVICE (bytes-phys used))
          (bytes-u16-set! common VIRTIO-QUEUE-ENABLE 1)
          (list qsize desc avail used)))))

;; The MAC sits in the first 6 bytes of the device-specific config region (valid
;; because we negotiate VIRTIO_NET_F_MAC).
(define (virtio-net-read-mac devcfg)
  (list (bytes-u8-ref devcfg 0) (bytes-u8-ref devcfg 1) (bytes-u8-ref devcfg 2)
        (bytes-u8-ref devcfg 3) (bytes-u8-ref devcfg 4) (bytes-u8-ref devcfg 5)))

;; --- bring-up ---------------------------------------------------------------

(define VIRTIO-NET-VID #x1af4)
(define VIRTIO-NET-DID #x1041)      ; modern virtio-net-pci
(define VIRTIO-NET-F-MAC-BIT 5)     ; feature bit 5 (low word)
(define VIRTIO-F-VERSION-1-BIT 0)   ; feature bit 32 -> bit 0 of the high word

(define (virtio-net-init)
  (let ((ecam (pci-find VIRTIO-NET-VID VIRTIO-NET-DID)))
    (if (not ecam)
        (begin (display "[virtio-net] no device present") (newline) #f)
        (let ((cfg (mmio-map ecam #x1000)))
          ;; enable PCI memory space + bus master
          (bytes-u16-set! cfg PCI-COMMAND (bitwise-or (bytes-u16-ref cfg PCI-COMMAND) #x6))
          (let ((common (map-cap cfg (find-virtio-cap cfg VIRTIO-CFG-COMMON)))
                (devcfg (map-cap cfg (find-virtio-cap cfg VIRTIO-CFG-DEVICE))))
            ;; reset, then ACKNOWLEDGE + DRIVER
            (bytes-u8-set! common VIRTIO-DEVICE-STATUS 0)
            (virtio-status-set! common VIRTIO-STATUS-ACK)
            (virtio-status-set! common VIRTIO-STATUS-DRIVER)
            ;; negotiate: accept only VIRTIO_NET_F_MAC (low) + VIRTIO_F_VERSION_1 (high)
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
                (let ((rxq (virtio-setup-queue common 0))    ; queue 0 = receiveq
                      (txq (virtio-setup-queue common 1)))   ; queue 1 = transmitq
                  (virtio-status-set! common VIRTIO-STATUS-DRIVER-OK)
                  (let ((mac (virtio-net-read-mac devcfg)))
                    (display "[virtio-net] up: mac=") (display mac)
                    (display " rxq-size=") (display (car rxq))
                    (display " txq-size=") (display (car txq))
                    (newline)
                    ;; the driver state RX/TX (N3/N4) will close over
                    (list 'virtio-net cfg common devcfg rxq txq mac)))))))))
