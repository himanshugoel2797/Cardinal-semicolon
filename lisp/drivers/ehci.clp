;; ehci: an EHCI (USB 2.0 high-speed) host-controller driver in Cardinal Lisp.
;; Like uhci/xhci it registers a host controller with coreusb -- a long-lived
;; CONTEXT answering control/bulk/interrupt transfer messages -- but over the EHCI
;; async schedule: one reusable Queue Head whose qTD overlay we re-arm per transfer,
;; with completion polled from the qTD status in DMA (no MSI), exactly as uhci does.
;;
;; EHCI root ports are HIGH-speed only; a full/low-speed device is released to a
;; companion controller (PortOwner). The periodic schedule (hardware-paced
;; interrupt/iso) and split transactions (FS/LS behind a HS hub) are not
;; implemented -- interrupt-in is serviced as a one-shot async IN, and iso replies
;; an error. The natural EHCI test device is therefore a high-speed one
;; (usb-storage).
;;
;; Capabilities captured at load (W7): sys-mmio (MMIO map + 32-bit DMA), sys-pci
;; (find + bus-master + BAR assign). The serve loop runs in a spawn-restricted '()
;; context closing over those captured primitives.
;;
;; Components (lisp/drivers/ehci/):
;;   regs   -- register map + Queue Head / qTD builders and readers.
;;   driver -- discovery/reset, the async transfer engine, port scan, HC loop.
(define-module ehci
  (export ehci-init)
  (import sys-mmio sys-pci driver-util coreusb)
  (define lg (make-logger 'ehci))
  (include regs driver))
