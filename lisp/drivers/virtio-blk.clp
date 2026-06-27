;; virtio-blk: a virtio 1.0 block-storage driver written in Cardinal Lisp.
;;
;; A sibling of virtio-net: the device-agnostic virtio transport (the PCI cap
;; walk, the status/feature handshake, split-virtqueue setup, the notify kick)
;; lives in the shared `virtio` library; this module is the block-SPECIFIC half:
;; the single request virtqueue, the 3-descriptor request idiom (header / data /
;; status), and the block-device driver context that answers corestorage
;; (read/write lba count ...) requests -- mirroring ahci/driver.clp's contract.
;;
;; Bind: init.clp enumerates virtio-blk PCI functions (1af4:1042 modern, also
;; 1af4:1001 transitional) and calls (virtio-blk-init storage <ecam>) for each.
;; virtio-blk-init does the bring-up in a SPAWNED restricted context (the init
;; caller is NOT under the scheduler, so a blocking recv there would wedge it)
;; and returns immediately; the spawned context registers with corestorage.

(define-module virtio-blk
  (export virtio-blk-init
          ;; pure helpers (host-testable)
          blk-build-header! blk-status-ok?
          VIRTIO-BLK-T-IN VIRTIO-BLK-T-OUT)
  (import sys-mmio sys-pci driver-util virtio)

;; --- virtio-blk request layout ---------------------------------------------
;; A request is a 3-descriptor chain:
;;   desc 0: 16-byte header, device-READABLE
;;             type    u32 @ 0   (0 = VIRTIO_BLK_T_IN/read, 1 = VIRTIO_BLK_T_OUT/write)
;;             reserved u32 @ 4
;;             sector  u64 @ 8   (the 512-byte LBA)
;;   desc 1: data buffer (count*512), device-WRITABLE for read / readable for write
;;   desc 2: 1-byte status, device-WRITABLE (0=OK, 1=IOERR, 2=UNSUPP)

(define VIRTIO-BLK-T-IN  0)   ; read from device
(define VIRTIO-BLK-T-OUT 1)   ; write to device
(define BLK-HDR-LEN     16)
(define SECTOR-SIZE    512)

;; PURE: fill a (>=16-byte) header buffer for a request of `type` at 512-byte
;; LBA `sector`. sector is a u64 -- composed from a low/high u32 pair, since the
;; byte accessors top out at u32 (bytes-u64-set! is the LE convenience that does
;; the same). Returns the buffer. Host-tested over a mock bytes region.
(define (blk-build-header! hdr type sector)
  (bytes-u32-set! hdr 0 type)
  (bytes-u32-set! hdr 4 0)                ; reserved
  (bytes-u64-set! hdr 8 sector)           ; LE: low word @8, high word @12
  hdr)

;; PURE: was the device's status byte OK? (0 = VIRTIO_BLK_S_OK; 1/2 = error.)
(define (blk-status-ok? status) (= status 0))

;; --- the request virtqueue (single outstanding request) --------------------
;; corestorage serialises through the driver context's mailbox, so exactly one
;; request is ever in flight: a fixed descriptor triple (0,1,2) needs no
;; allocator, and polling the used ring (no MSI) is sufficient and simplest.

(define DATA-SECTORS 8)   ; cap a single request at 8 sectors (4KB) -- sizes the DMA buffer
(define IO-TIMEOUT-NS 2000000000)   ; 2s: a stuck device fails the request rather than hangs

;; Issue one request and wait for completion. `dir` is VIRTIO-BLK-T-IN/OUT.
;;   hdr/hdr-phys   : 16-byte header buffer (filled here)
;;   data-phys      : physical base of the data buffer (count*512 bytes)
;;   stat/stat-phys : 1-byte status buffer (device writes it)
;; Returns 0 on success, -1 on error/timeout.
(define (blk-request! q notify mult dir lba count
                      hdr hdr-phys data-phys stat stat-phys)
  (blk-build-header! hdr dir lba)
  (bytes-u8-set! stat 0 #xFF)                 ; poison: device overwrites with 0/1/2
  (let ((desc (q-desc q)) (avail (q-avail q)) (qsize (q-size q))
        (last (bytes-u16-ref (q-used q) 2))
        ;; data desc is device-WRITABLE for a read, plain (device-readable) for a write
        (data-flags (if (= dir VIRTIO-BLK-T-IN)
                        (bitwise-or VIRTQ-DESC-F-NEXT VIRTQ-DESC-F-WRITE)
                        VIRTQ-DESC-F-NEXT)))
    (desc-set! desc 0 hdr-phys  BLK-HDR-LEN          VIRTQ-DESC-F-NEXT 1)
    (desc-set! desc 1 data-phys (* count SECTOR-SIZE) data-flags       2)
    (desc-set! desc 2 stat-phys 1                    VIRTQ-DESC-F-WRITE 0)
    (avail-push! avail qsize 0)
    (notify-queue! notify mult q)
    ;; Single outstanding request -> polling used.idx is fine. Busy-spin ~500us
    ;; first (a paravirt completion is microseconds), then yield.
    (if (wait-until-spin (lambda () (used-advanced? q last)) IO-TIMEOUT-NS 500000)
        (if (blk-status-ok? (bytes-u8-ref stat 0)) 0 -1)
        -1)))

;; --- block-device driver context -------------------------------------------
;; Answers corestorage exactly as ahci's make-driver-ctx does:
;;   (read  lba count reply) -> (send reply (list 'complete status bytes|#f))
;;   (write lba count data  reply) -> (send reply (list 'complete status))
;; The DMA buffers are sized for one DATA-SECTORS request; count is capped here
;; (corestorage bounds lba+count against bcount, not count alone).
(define (make-driver-ctx q notify mult hdr hdr-phys data-buf data-phys stat stat-phys)
  (spawn-restricted '()
    (lambda ()
      (let loop ()
        (let ((m (recv)))
          (cond
            ((eq? (car m) 'read)
             (let ((lba (cadr m)) (count (caddr m)) (reply (cadddr m)))
               (if (> count DATA-SECTORS)
                   (send reply (list 'complete -1 #f))
                   (let ((st (blk-request! q notify mult VIRTIO-BLK-T-IN lba count
                                           hdr hdr-phys data-phys stat stat-phys)))
                     (send reply
                           (list 'complete st
                                 (if (= st 0) (copy-bytes data-buf 0 (* count SECTOR-SIZE)) #f)))))))
            ((eq? (car m) 'write)
             (let ((lba (cadr m)) (count (caddr m))
                   (data (cadddr m)) (reply (nth m 4)))
               (if (> count DATA-SECTORS)
                   (send reply (list 'complete -1))
                   (begin
                     (bytes-copy-into! data-buf 0 data (* count SECTOR-SIZE))
                     (send reply
                           (list 'complete
                                 (blk-request! q notify mult VIRTIO-BLK-T-OUT lba count
                                               hdr hdr-phys data-phys stat stat-phys))))))))
          (loop))))))

;; --- device-config capacity ------------------------------------------------
;; capacity (sectors) is a u64 at devcfg offset 0: low u32 @0 | high u32 @4.
;; Returned as a plain fixnum (kernel fixnums are 64-bit, so a realistic
;; capacity fits); the high word is shifted in for completeness.
(define (blk-capacity devcfg)
  (+ (bytes-u32-ref devcfg 0)
     (arithmetic-shift (bytes-u32-ref devcfg 4) 32)))

;; --- bring-up ---------------------------------------------------------------
;; Run in a spawned, yielding context (it allocates rings + registers). For blk
;; we want only VIRTIO-F-VERSION-1 (high word bit 0): lo-want=0.
(define (virtio-blk-bringup storage ecam name)
  (let ((dev (virtio-bringup ecam 0 (arithmetic-shift 1 VIRTIO-F-VERSION-1-BIT))))
    (if (not dev)
        (begin (display "[virtio-blk] device rejected FEATURES_OK") (newline) 'fail)
        (let ((common (nth dev 0)) (devcfg (nth dev 1))
              (notify (nth dev 2)) (mult (nth dev 3)))
          (let ((q (virtio-setup-queue common 0)))    ; the single request queue
            (if (not q)
                (begin (display "[virtio-blk] no request queue") (newline) 'fail)
                (let ((capacity (blk-capacity devcfg)))
                  (virtio-status-set! common VIRTIO-STATUS-DRIVER-OK)
                  (display "[virtio-blk] up: sectors=") (display capacity) (newline)
                  ;; DMA: one header, one DATA-SECTORS data buffer, one status byte.
                  (let* ((hdr  (dma-alloc BLK-HDR-LEN))
                         (data (dma-alloc (* DATA-SECTORS SECTOR-SIZE)))
                         (stat (dma-alloc 1))
                         (drv  (make-driver-ctx q notify mult
                                                hdr  (bytes-phys hdr)
                                                data (bytes-phys data)
                                                stat (bytes-phys stat))))
                    ;; bsize = 512 (sector size); lba maps directly to sector.
                    (send storage (list 'register-blockdev name 512 capacity drv))
                    (display "[virtio-blk] registered ") (display name)
                    (display " with corestorage") (newline)
                    'up))))))))

;; virtio-blk-init: the entry point init.clp calls (per matching device). It
;; spawns the bring-up context (fire-and-forget, mirroring virtio-net/ahci) so
;; the not-under-scheduler init caller never blocks, and returns immediately.
;; `name` defaults to 'vblk0; init may pass a distinct symbol per device.
(define (virtio-blk-init storage ecam)
  (if (not ecam)
      (begin (display "[virtio-blk] no device present") (newline) #f)
      (begin
        (spawn-restricted '()
          (lambda () (virtio-blk-bringup storage ecam 'vblk0)))
        'virtio-blk-spawned)))

) ; close (define-module virtio-blk ...)
