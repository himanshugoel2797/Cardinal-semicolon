;; virtio-gpu: a 2D virtio-gpu display driver in Cardinal Lisp, ported from
;; drivers/virtio/gpu. Brings the device to DRIVER_OK, reads the display geometry,
;; creates a scanout-backing framebuffer for each enabled display, paints it, and
;; registers the result with the coredisplay service.
;;
;; Built on the shared `virtio` transport (the same library virtio-net uses): the
;; PCI capability walk, the status/feature handshake and the split virtqueue setup
;; are all there. What is GPU-specific lives in the include parts:
;;
;;   cmds    -- the control-queue command builders + response accessors. Unlike
;;              network headers, GPU command structs are LITTLE-endian, so they use
;;              the native bytes-uNN-set!/-ref, not the big-endian put-be*!.
;;   bringup -- the ordered handshake: get-display-info, then per scanout
;;              create-2d / attach-backing / set-scanout / transfer / flush.
;;   driver  -- the long-lived driver context (recv loop) + virtio-gpu-init.
;;
;; Unlike virtio-net this driver POLLS the control queue's used ring (it is DMA-
;; coherent uncached) rather than taking an MSI: the controlq is strictly
;; request/response and serial, so ctrlq-cmd! just waits for used.idx to advance.
;; MSI is deferred to a future display-resize event path.
;;
;; The control queue is single-command-in-flight, so this module is NOT a server
;; that fans out concurrent commands; it is one bring-up context plus a small
;; recv loop answering (flush)/(get-framebuffer) requests serially.

(define-module virtio-gpu
  ;; virtio-gpu-init is the driver entry point; the pure command builders + the
  ;; ctrlq round-trip are also exported so the in-OS self-test can pin the GPU
  ;; struct offsets (a hardware-free regression net) without a device.
  (export virtio-gpu-init
          make-create-2d make-attach-backing make-set-scanout
          make-transfer-2d make-flush make-display-info-cmd
          gpu-resp-type resp-display-enabled? resp-display-width resp-display-height
          GPU-RESP-MAX GPU-RESP-OK-NODATA GPU-RESP-OK-DISPLAY-INFO)
  (import sys-mmio sys-pci driver-util virtio)
  (include cmds bringup driver))
