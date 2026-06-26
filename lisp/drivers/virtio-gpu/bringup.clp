;; virtio-gpu/bringup: the ordered GPU handshake.
;;
;; NOT a module -- spliced into virtio-gpu by `include`; shares its imports and
;; the cmds part. Runs inside a spawned context (so its wait-until/sleep yield).

(define VIRTIO-GPU-VID #x1AF4)
(define VIRTIO-GPU-DID #x1050)

;; The controlq is queue 0. Commands are serial, so one descriptor pair suffices.
(define GPU-CTRLQ 0)
(define GPU-CURSORQ 1)
(define GPU-CMD-TIMEOUT-NS 1000000000)   ; 1s -- generous for a paravirt device

;; Issue one control-queue command and return its response buffer (or #f on
;; timeout). cmd is a freshly-built command buffer; we allocate a DMA response
;; buffer big enough for any GPU reply. Both must be DMA-addressable, so copy the
;; command into a DMA buffer first (make-* builds a CPU buffer).
(define (gpu-cmd! ctrlq notify mult cmd cmd-len)
  (let ((dcmd (dma-alloc cmd-len))
        (resp (dma-alloc GPU-RESP-MAX)))
    (bytes-copy-into! dcmd 0 cmd cmd-len)
    (ctrlq-cmd! ctrlq notify mult dcmd cmd-len resp GPU-RESP-MAX GPU-CMD-TIMEOUT-NS)))

;; Issue a control command that expects a bare OK_NODATA reply, and warn if the
;; device answered with anything else (an error type, or a timeout #f). The
;; bring-up still proceeds -- a paravirt device rarely errors here -- but a silent
;; black scanout is hard to debug, so a mismatch is at least logged. `what` names
;; the command for the log.
(define (gpu-cmd-ok! ctrlq notify mult cmd cmd-len what)
  (let ((r (gpu-cmd! ctrlq notify mult cmd cmd-len)))
    (if (or (not r) (not (= (gpu-resp-type r) GPU-RESP-OK-NODATA)))
        (begin (display "[virtio-gpu] ") (display what)
               (display " unexpected response: ")
               (display (if r (gpu-resp-type r) 'timeout)) (newline)))
    r))

;; Paint a freshly-allocated framebuffer to a solid value (0xFF -> white in
;; X8R8G8B8: the X byte is ignored, R=G=B=0xFF). The fill is byte-uniform, so
;; replicate `val` into a 32-bit word and clear the whole buffer in one bulk
;; bytes-fill32! (vectorized C) instead of nbytes interpreted bytes-u8-set! calls
;; -- a 1080p framebuffer goes from ~8M VM iterations to a single primitive.
;; nbytes is always w*h*4 (a multiple of 4).
(define (fill-fb! fb nbytes val)
  (let ((color (bitwise-or val
                           (arithmetic-shift val 8)
                           (arithmetic-shift val 16)
                           (arithmetic-shift val 24))))
    (bytes-fill32! fb 0 (quotient nbytes 4) color)
    fb))

;; Bring one scanout up: create a 2D resource, allocate + paint its framebuffer,
;; attach it as backing, point the scanout at it, and push the first frame
;; (transfer + flush). Returns (list scanout-idx res-id w h fb) on success, or #f
;; if the framebuffer allocation failed (caller logs + bails without registering).
;;
;; The framebuffer is a single contiguous WRITE-BACK (cached) dma-alloc-wb: the
;; CPU composes into it at cache speed and the device reads it coherently (x86 DMA
;; is cache-coherent; the flush path fences before signalling). WB -- not the usual
;; uncached dma-alloc -- because this buffer is CPU-write/device-read only, never a
;; CPU-polled device-write ring. attach-backing here uses one mem entry; a chunked
;; (scatter-gather) attach for very large modes is a TODO.
(define (init-scanout ctrlq notify mult scanout-idx res-id w h)
  (let* ((nbytes (* w h 4))
         (cr (gpu-cmd-ok! ctrlq notify mult
                          (make-create-2d res-id GPU-FORMAT-X8R8G8B8 w h) 40 'create-2d)))
    ;; If create-2d failed (gpu-cmd-ok! already logged it), bail -- the later
    ;; commands all reference res-id, which the device never created, so they
    ;; would NACK in a loop and leave the scanout in a confused state.
    (if (not (and cr (= (gpu-resp-type cr) GPU-RESP-OK-NODATA)))
        #f
        (let ((fb (dma-alloc-wb nbytes)))
          (if (not fb)
              (begin (display "[virtio-gpu] framebuffer alloc failed") (newline) #f)
              (begin
                (fill-fb! fb nbytes #xFF)
            (gpu-cmd-ok! ctrlq notify mult
                         (make-attach-backing res-id (bytes-phys fb) nbytes) 48 'attach-backing)
            (gpu-cmd-ok! ctrlq notify mult
                         (make-set-scanout scanout-idx res-id 0 0 w h) 48 'set-scanout)
            (gpu-cmd-ok! ctrlq notify mult
                         (make-transfer-2d res-id 0 0 0 w h) 56 'transfer-2d)
            (gpu-cmd-ok! ctrlq notify mult (make-flush res-id 0 0 w h) 48 'flush)
            (list scanout-idx res-id w h fb)))))))

;; The full bring-up: transport bring-up, queues, DRIVER_OK, then read display
;; info and initialise every enabled scanout. Returns a device record
;;   (list ctrlq notify mult scanouts)
;; -- the controlq + notify handles (so the driver loop can re-flush) and the list
;; of scanout records (each (idx res-id w h fb)). Returns #f if the device is
;; absent / rejected features / no scanout came up.
(define (gpu-bringup)
  (let ((ecam (pci-find VIRTIO-GPU-VID VIRTIO-GPU-DID)))
    (if (not ecam)
        (begin (display "[virtio-gpu] no device present") (newline) #f)
        ;; want only VERSION_1 (hi); no low-word features (no VirGL in 2D mode).
        (let ((dev (virtio-bringup ecam 0 (arithmetic-shift 1 VIRTIO-F-VERSION-1-BIT))))
          (if (not dev)
              (begin (display "[virtio-gpu] device rejected FEATURES_OK") (newline) #f)
              (let ((common (nth dev 0)) (notify (nth dev 2)) (mult (nth dev 3)))
                (let ((ctrlq (virtio-setup-queue common GPU-CTRLQ)))
                  (virtio-setup-queue common GPU-CURSORQ)   ; parity with the C driver
                  (virtio-status-set! common VIRTIO-STATUS-DRIVER-OK)
                  ;; Read display geometry.
                  (let ((info (gpu-cmd! ctrlq notify mult (make-display-info-cmd) GPU-HDR-LEN)))
                    (if (not info)
                        (begin (display "[virtio-gpu] GET_DISPLAY_INFO timed out") (newline) #f)
                        ;; Init each enabled scanout; assign resource ids from 1.
                        (let loop ((i 0) (res-id 1) (acc '()))
                          (if (>= i 16)
                              (begin
                                (display "[virtio-gpu] up: scanouts=")
                                (display (reverse acc)) (newline)
                                (list ctrlq notify mult (reverse acc)))
                              (if (resp-display-enabled? info i)
                                  (let* ((w (resp-display-width info i))
                                         (h (resp-display-height info i))
                                         (sc (init-scanout ctrlq notify mult i res-id w h)))
                                    (if sc
                                        (loop (+ i 1) (+ res-id 1) (cons sc acc))
                                        (loop (+ i 1) res-id acc)))   ; alloc failed: skip
                                  (loop (+ i 1) res-id acc)))))))))))))
