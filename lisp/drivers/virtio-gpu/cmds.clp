;; virtio-gpu/cmds: GPU control-queue command builders + response accessors.
;;
;; NOT a module -- spliced into virtio-gpu by `include`; it shares that module's
;; imports (driver-util's byte helpers, etc.).
;;
;; CRITICAL: every virtio-gpu struct field is LITTLE-endian, so these use the
;; native volatile accessors (bytes-u32-set!/bytes-u64-set!) -- NEVER the
;; big-endian put-be*! helpers (those are for on-the-wire network headers). The
;; offsets below mirror drivers/virtio/gpu/inc/virtio_gpu.h field for field.

;; --- control header (24 bytes): every command/response begins with this -------
;;   type:u32@0  flags:u32@4  fence_id:u64@8  ctx_id:u32@16  padding:u32@20
(define GPU-HDR-LEN 24)

(define (gpu-hdr-set! b type)
  (bytes-u32-set! b 0 type)
  (bytes-u32-set! b 4 0)            ; flags
  (bytes-u64-set! b 8 0)            ; fence_id
  (bytes-u32-set! b 16 0)           ; ctx_id
  (bytes-u32-set! b 20 0))          ; padding

;; --- command type codes -------------------------------------------------------
(define GPU-CMD-GET-DISPLAY-INFO       #x0100)
(define GPU-CMD-RESOURCE-CREATE-2D     #x0101)
(define GPU-CMD-SET-SCANOUT            #x0103)
(define GPU-CMD-RESOURCE-FLUSH         #x0104)
(define GPU-CMD-TRANSFER-TO-HOST-2D    #x0105)
(define GPU-CMD-RESOURCE-ATTACH-BACKING #x0106)

;; --- response type codes ------------------------------------------------------
(define GPU-RESP-OK-NODATA       #x1100)
(define GPU-RESP-OK-DISPLAY-INFO  #x1101)

;; --- pixel format -------------------------------------------------------------
(define GPU-FORMAT-X8R8G8B8 4)

;; --- a rect at offset `o`: x,y,width,height (u32 each, 16 bytes) ---------------
(define (gpu-rect-set! b o x y w h)
  (bytes-u32-set! b o        x)
  (bytes-u32-set! b (+ o 4)  y)
  (bytes-u32-set! b (+ o 8)  w)
  (bytes-u32-set! b (+ o 12) h))

;; RESOURCE_CREATE_2D (40 bytes): resource_id@24, format@28, width@32, height@36.
(define (make-create-2d res-id fmt w h)
  (let ((b (make-bytes 40)))
    (gpu-hdr-set! b GPU-CMD-RESOURCE-CREATE-2D)
    (bytes-u32-set! b 24 res-id)
    (bytes-u32-set! b 28 fmt)
    (bytes-u32-set! b 32 w)
    (bytes-u32-set! b 36 h)
    b))

;; RESOURCE_ATTACH_BACKING (48 bytes, single mem entry): resource_id@24,
;; nr_entries@28 (=1), entry{addr:u64@32, length@40, padding@44}.
(define (make-attach-backing res-id addr length)
  (let ((b (make-bytes 48)))
    (gpu-hdr-set! b GPU-CMD-RESOURCE-ATTACH-BACKING)
    (bytes-u32-set! b 24 res-id)
    (bytes-u32-set! b 28 1)          ; nr_entries
    (bytes-u64-set! b 32 addr)
    (bytes-u32-set! b 40 length)
    (bytes-u32-set! b 44 0)          ; entry padding
    b))

;; SET_SCANOUT (48 bytes): rect@24 (x,y,w,h), scanout_id@40, resource_id@44.
(define (make-set-scanout scanout-id res-id x y w h)
  (let ((b (make-bytes 48)))
    (gpu-hdr-set! b GPU-CMD-SET-SCANOUT)
    (gpu-rect-set! b 24 x y w h)
    (bytes-u32-set! b 40 scanout-id)
    (bytes-u32-set! b 44 res-id)
    b))

;; TRANSFER_TO_HOST_2D (56 bytes): rect@24, offset:u64@40, resource_id@48,
;; padding@52.
(define (make-transfer-2d res-id offset x y w h)
  (let ((b (make-bytes 56)))
    (gpu-hdr-set! b GPU-CMD-TRANSFER-TO-HOST-2D)
    (gpu-rect-set! b 24 x y w h)
    (bytes-u64-set! b 40 offset)
    (bytes-u32-set! b 48 res-id)
    (bytes-u32-set! b 52 0)          ; padding
    b))

;; RESOURCE_FLUSH (48 bytes): rect@24, resource_id@40, padding@44.
(define (make-flush res-id x y w h)
  (let ((b (make-bytes 48)))
    (gpu-hdr-set! b GPU-CMD-RESOURCE-FLUSH)
    (gpu-rect-set! b 24 x y w h)
    (bytes-u32-set! b 40 res-id)
    (bytes-u32-set! b 44 0)          ; padding
    b))

;; GET_DISPLAY_INFO (24 bytes: header only).
(define (make-display-info-cmd)
  (let ((b (make-bytes GPU-HDR-LEN)))
    (gpu-hdr-set! b GPU-CMD-GET-DISPLAY-INFO)
    b))

;; --- response accessors -------------------------------------------------------
;; resp_display_info: hdr(24) then pmodes[16], each 24 bytes:
;;   rect{x@0,y@4,width@8,height@12}  enabled:u32@16  flags:u32@20.
;; pmode[i] starts at 24 + 24*i.
(define (resp-display-pmode-off i) (+ 24 (* 24 i)))
(define (resp-display-enabled? resp i)
  (not (= 0 (bytes-u32-ref resp (+ (resp-display-pmode-off i) 16)))))
(define (resp-display-width resp i)
  (bytes-u32-ref resp (+ (resp-display-pmode-off i) 8)))
(define (resp-display-height resp i)
  (bytes-u32-ref resp (+ (resp-display-pmode-off i) 12)))

;; A response buffer big enough for the largest reply (the display-info struct:
;; 24-byte hdr + 16 pmodes * 24 bytes = 408). All other replies are a bare hdr.
(define GPU-RESP-MAX 408)
(define (gpu-resp-type resp) (bytes-u32-ref resp 0))
