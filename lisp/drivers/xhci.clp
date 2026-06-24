;; xhci: an xHCI (USB 3) host-controller driver in Cardinal Lisp, ported from
;; drivers/xhci. Like uhci it registers a host controller with coreusb -- a
;; long-lived context answering transfer messages -- but xHCI is MMIO + MSI and
;; manages addressing through Enable Slot / Address Device commands and TRB rings.
;;
;; Capabilities captured at load (W7): sys-mmio (MMIO map + 32-bit DMA), sys-pci
;; (find + bus-master + MSI + BAR assign). The serve loop runs in a
;; spawn-restricted '() context closing over those captured primitives. It uses
;; the per-device MSI (msi-wait) to yield while waiting on the event ring, falling
;; back to polling the ring directly so a missed interrupt cannot wedge a transfer.
;;
;; Components (lisp/drivers/xhci/):
;;   regs   -- register map, TRB type helpers, the producer TRB ring.
;;   driver -- init/reset, slot/address model, command + event rings, transfers,
;;             hub support, and the HC context loop.
(define-module xhci
  (export xhci-init)
  (import sys-mmio sys-pci driver-util coreusb)
  (include regs driver))
