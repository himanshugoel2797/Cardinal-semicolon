;; uhci: a UHCI (USB 1.1) host-controller driver in Cardinal Lisp, ported from
;; drivers/uhci. It registers a host controller with coreusb (proto.clp): a
;; long-lived CONTEXT that answers control/interrupt/bulk transfer messages by
;; building TD chains in a DMA scratch page and polling them to completion, and
;; that polls its two root ports for hotplug.
;;
;; Capabilities captured at load (W7): sys-io (the registers live at an I/O BAR),
;; sys-mmio (config-space map + 32-bit DMA scratch/frame list), sys-pci (find +
;; bus-master enable). It runs its serve loop in a spawn-restricted '() context,
;; which holds no grant but closes over those captured primitives.
;;
;; Components (lisp/drivers/uhci/):
;;   regs   -- register map, port-I/O helpers, TD/QH field builders + readers.
;;   xfer   -- the control and interrupt/bulk transfer engines (build + poll).
;;   driver -- discovery, reset, port reset/scan, and the HC context loop.
(define-module uhci
  (export uhci-init)
  (import sys-io sys-mmio sys-pci driver-util coreusb)
  (define lg (make-logger 'uhci))
  (include regs xfer driver))
