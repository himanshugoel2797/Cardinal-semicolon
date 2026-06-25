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
          usb-isoch-out usb-isoch-in
          usb-find-endpoint usb-iface-class usb-iface-subclass usb-iface-protocol
          usb-iface-number
          usb-mark-hub usb-enumerate-downstream usb-disconnect-downstream
          usb-dev-hci usb-dev-address usb-dev-speed usb-dev-mps0 usb-dev-config usb-dev-config-len
          complete-n complete-data
          ;; full descriptor model (multi-interface / alt-setting aware)
          usb-interfaces iface-number iface-alt iface-class iface-subclass
          iface-protocol iface-num-eps iface-offset
          usb-iface-endpoints usb-find-ep-in
          ep-address ep-attributes ep-type ep-sync-type ep-dir-in? ep-number
          ep-max-packet ep-interval
          ;; standard requests + strings
          usb-get-descriptor usb-set-interface
          usb-string usb-string-raw usb-string-decode usb-langid
          ;; robustness: retry + endpoint-halt recovery
          with-retries usb-clear-halt USB-FEATURE-ENDPOINT-HALT make-setup
          ;; request/descriptor/class/speed constants used by class drivers
          USB-REQ-DIR-IN USB-REQ-DIR-OUT USB-REQ-TYPE-CLASS
          USB-REQ-RECIP-DEVICE USB-REQ-RECIP-INTERFACE USB-REQ-RECIP-ENDPOINT
          USB-REQ-RECIP-OTHER
          USB-REQ-GET-STATUS USB-REQ-CLEAR-FEATURE USB-REQ-SET-FEATURE
          USB-REQ-GET-DESCRIPTOR USB-REQ-SET-INTERFACE
          USB-REQ-SET-ADDRESS USB-REQ-SET-CONFIGURATION
          USB-DESC-DEVICE USB-DESC-CONFIG USB-DESC-STRING USB-DESC-INTERFACE
          USB-DESC-ENDPOINT USB-DESC-IFACE-ASSOC
          USB-CLASS-AUDIO USB-CLASS-CDC USB-CLASS-HID USB-CLASS-MASS-STORAGE
          USB-CLASS-HUB
          USB-XFER-CONTROL USB-XFER-ISOCH USB-XFER-BULK USB-XFER-INTERRUPT
          USB-SPEED-LOW USB-SPEED-FULL USB-SPEED-HIGH USB-SPEED-SUPER)
  (import driver-util)
  (include proto enum))
