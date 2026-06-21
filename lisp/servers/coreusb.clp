;; coreusb: the USB enumeration + class-dispatch service, ported from
;; servers/CoreUsb. The kernel C stack registered host controllers and class
;; drivers through function-pointer tables and drove enumeration with synchronous
;; transfer calls; here a host controller is a CONTEXT that answers transfer
;; messages (see proto.clp for the protocol) and coreusb is a `serve` registry
;; that, on a port connect, spawns an enumerator context to run the USB 2.0 ch.9
;; sequence and hand the device to the registered class driver.
;;
;; coreusb holds NO hardware capability -- it never touches MMIO/DMA/PCI; it only
;; routes messages and parses descriptor bytes (ambient byte-buffer ops). The
;; controllers it talks to captured their own sys-* authority at load time.
;;
;; Components (lisp/servers/coreusb/):
;;   proto -- USB wire constants, setup-packet builder, the host-controller
;;            transfer message protocol, and the class-driver-facing transfer API
;;            + descriptor walkers (find-endpoint / interface fields).
;;   enum  -- the enumeration state machine, the bus-address pool, the enumerated
;;            -device records, and the coreusb registry service.
(define-module coreusb
  (export start-usb-service
          ;; class-driver transfer API + descriptor accessors
          usb-control-in usb-control-out usb-interrupt-in usb-bulk-in usb-bulk-out
          usb-find-endpoint usb-iface-class usb-iface-protocol usb-iface-number
          usb-dev-address usb-dev-speed usb-dev-config usb-dev-config-len
          ;; request/descriptor/class/speed constants used by class drivers
          USB-REQ-DIR-IN USB-REQ-DIR-OUT USB-REQ-TYPE-CLASS
          USB-REQ-RECIP-INTERFACE USB-REQ-RECIP-OTHER
          USB-REQ-GET-STATUS USB-REQ-CLEAR-FEATURE USB-REQ-SET-FEATURE
          USB-REQ-GET-DESCRIPTOR USB-REQ-SET-INTERFACE
          USB-DESC-DEVICE USB-DESC-CONFIG USB-DESC-INTERFACE USB-DESC-ENDPOINT
          USB-CLASS-HID USB-CLASS-MASS-STORAGE USB-CLASS-HUB
          USB-XFER-BULK USB-XFER-INTERRUPT
          USB-SPEED-LOW USB-SPEED-FULL USB-SPEED-HIGH USB-SPEED-SUPER)
  (import driver-util)
  (include proto enum))
