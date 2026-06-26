;; virtio: the device-agnostic virtio 1.0 (modern, PCI-capability) transport.
;;
;; Factored out of the original virtio-net driver so every virtio device (net,
;; gpu, ...) shares one copy of the bring-up dance: walk the PCI capabilities to
;; find the COMMON / NOTIFY / DEVICE config regions, drive the device-status
;; handshake, negotiate features, and set up split virtqueues. The device-
;; SPECIFIC code (feature bits, queue roles, config-space layout, command
;; framing) stays in each driver module, which (import virtio)s this.
;;
;; A "device record" returned by virtio-bringup is the list
;;   (common devcfg notify mult ecam)
;; -- the three mapped MMIO regions, the notify-offset multiplier, and the ecam
;; physical base (kept so a driver can still reach pci-setup-msi if it wants
;; interrupts; the GPU driver polls instead).
;;
;; A queue record (from virtio-setup-queue) is (size desc avail used notify-off).
;;
;; All multi-byte virtio register/ring fields are LITTLE-endian (the volatile
;; byte accessors are native LE), so this module uses bytes-uNN-set!/-ref
;; directly -- it never touches the big-endian put-be*! helpers (those are for
;; on-the-wire network headers and live in driver-util).

(define-module virtio
  (export
    ;; PCI / capability walk
    find-virtio-cap bar-base map-cap
    PCI-COMMAND VIRTIO-CFG-COMMON VIRTIO-CFG-NOTIFY VIRTIO-CFG-DEVICE
    ;; status / feature handshake
    virtio-status-set! virtio-features-offered virtio-features-accept!
    VIRTIO-STATUS-ACK VIRTIO-STATUS-DRIVER VIRTIO-STATUS-DRIVER-OK
    VIRTIO-STATUS-FEATURES-OK VIRTIO-F-VERSION-1-BIT
    ;; split virtqueue
    VIRTQ-DESC-F-NEXT VIRTQ-DESC-F-WRITE
    desc-set! avail-push! q-size q-desc q-avail q-used q-noff
    virtio-setup-queue notify-queue!
    ;; high-level helpers
    virtio-bringup ctrlq-cmd! used-advanced?
    ;; common-config register offsets (re-exported so drivers can poke
    ;; device-status etc. directly when they need DRIVER_OK timing control)
    VIRTIO-DEVICE-STATUS VIRTIO-MSIX-CONFIG)
  (import sys-mmio sys-pci driver-util)

;; --- PCI config space + virtio capability walk ------------------------------
;; PCI-COMMAND and bar-base are generic PCI plumbing -- they now live in
;; driver-util (imported above) and are re-exported here so existing
;; (import virtio) users still see them.

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

(define VIRTIO-F-VERSION-1-BIT 0)

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

;; --- common bring-up --------------------------------------------------------
;; The shared "discover + negotiate" dance. Maps the three config regions, drives
;; the status handshake, and negotiates features: for each 32-bit feature word
;; (select 0 = bits 0..31, select 1 = bits 32..63) it accepts (offered & want).
;; The caller passes the masks it wants -- net wants F_MAC|VERSION_1, gpu wants
;; only VERSION_1 -- so the device-specific bit knowledge stays out of here.
;; Returns the device record (common devcfg notify mult ecam), or #f if the
;; device rejected FEATURES_OK.
(define (virtio-bringup ecam lo-want hi-want)
  (let ((cfg (mmio-map ecam #x1000)))
    (pci-enable-mem-bus-master! cfg)
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
        (virtio-features-accept! common 0 (bitwise-and f-lo lo-want))
        (virtio-features-accept! common 1 (bitwise-and f-hi hi-want)))
      (virtio-status-set! common VIRTIO-STATUS-FEATURES-OK)
      (if (= 0 (bitwise-and (bytes-u8-ref common VIRTIO-DEVICE-STATUS)
                            VIRTIO-STATUS-FEATURES-OK))
          #f
          (list common devcfg notify mult ecam)))))

;; --- control-queue request/response -----------------------------------------
;; The 2-descriptor command/response idiom used by control-style queues (the GPU
;; controlq, the net controlq, ...). Descriptor 0 is the device-READABLE command
;; buffer (flagged NEXT -> descriptor 1); descriptor 1 is the device-WRITABLE
;; response buffer. The control queue is strictly serial (one command in flight),
;; so a fixed descriptor pair (0,1) is fine -- no allocator needed.
;;
;; Snapshot used.idx, post the pair on the avail ring, kick the device, then wait
;; (yielding) for used.idx to advance. Returns the response buffer (the caller
;; reads the device's reply out of it), or #f on timeout.

;; Has the used ring advanced past the snapshot `last`?
(define (used-advanced? q last)
  (not (= (bytes-u16-ref (q-used q) 2) last)))

(define (ctrlq-cmd! q notify mult cmd cmd-len resp resp-len timeout-ns)
  (let ((desc (q-desc q)) (avail (q-avail q)) (qsize (q-size q))
        (last (bytes-u16-ref (q-used q) 2)))
    (desc-set! desc 0 (bytes-phys cmd)  cmd-len  VIRTQ-DESC-F-NEXT 1)
    (desc-set! desc 1 (bytes-phys resp) resp-len VIRTQ-DESC-F-WRITE 0)
    (avail-push! avail qsize 0)
    (notify-queue! notify mult q)
    ;; busy-spin first (~500us): a controlq command completes in microseconds, and
    ;; a pure sleep-poll would pay a scheduler quantum of latency per command.
    (if (wait-until-spin (lambda () (used-advanced? q last)) timeout-ns 500000)
        resp
        #f))))
