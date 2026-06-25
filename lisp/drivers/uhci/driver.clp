;; uhci/driver -- controller discovery, reset, port reset/scan, and the
;; host-controller CONTEXT that serves transfer messages + polls its ports.
;; Ported from module_init / uhci_reset / uhci_enableport / intr_handler.
;;
;; UHCI has no usable interrupt here (the PIIX/ICH9 function exposes no MSI and
;; its INTx# GSI needs ACPI _PRT parsing the kernel lacks), so the C driver ran a
;; cooperative poll task; transfer completion is read straight from DMA-written TD
;; status. The Lisp port keeps that: ONE context both serves transfer-request
;; messages (proto.clp) and polls the root ports for hotplug, yielding via sleep.

;; pci-find matches VENDOR/DEVICE id only (no class-code find in the substrate
;; yet -- the same caveat ahci notes), so we try the common QEMU/PC UHCI ids and
;; bind the first present: piix3 (8086:7020), piix4 (8086:7112), ICH9 UHCI #1..3.
(define UHCI-IDS (list (list #x8086 #x7020) (list #x8086 #x7112)
                       (list #x8086 #x2934) (list #x8086 #x2935) (list #x8086 #x2936)))
(define (find-uhci ids)
  (if (null? ids) #f
      (let ((e (pci-find (caar ids) (cadr (car ids)))))
        (if e e (find-uhci (cdr ids))))))

(define POLL-INTERVAL 250000000)   ; 250ms root-port poll cadence
(define IDLE-NS         2000000)   ; 2ms nap between idle checks

;; Global + HC reset, then point the frame-list base register at our frame list
;; and set Run. Bounded wall-clock waits (yield) -- no iteration-count spins.
(define (uhci-reset iobar framelist-phys)
  (u-w16 iobar USBCMD-REG USBCMD-GRESET)
  (sleep 10000000)                                 ; 10ms global reset
  (u-w16 iobar USBCMD-REG 0)
  (u-w16 iobar USBCMD-REG USBCMD-HCRESET)
  (wait-until (lambda () (= 0 (bitwise-and (u-r16 iobar USBCMD-REG) USBCMD-HCRESET)))
              100000000)
  (u-w32 iobar FRBASEADDR-REG framelist-phys)
  (u-w16 iobar USBCMD-REG USBCMD-RS))              ; Run

;; Reset a root port: assert >=10ms, de-assert + recovery delay, then enable.
(define (enable-port! iobar idx)
  (u-w16 iobar (PORTSC-REG idx) PORTSC-PORTRESET)
  (sleep 15000000)
  (u-w16 iobar (PORTSC-REG idx) 0)
  (sleep 20000000)
  (u-w16 iobar (PORTSC-REG idx) (bitwise-or PORTSC-PORTEN PORTSC-PORTENCHG))
  (sleep 20000000))

;; Scan each root port; on a new connect, reset+enable it and tell coreusb (with
;; this context as the controller handle); on a new disconnect, tell coreusb.
;; Returns the updated per-port "enumerated?" list. Runs in the HC context, so
;; (self) is the controller handle coreusb sends transfers back to.
(define (poll-ports! iobar usb port-enum)
  (let loop ((i 0) (pe port-enum) (acc '()))
    (if (= i PORT-COUNT)
        (reverse acc)
        (let* ((p (u-r16 iobar (PORTSC-REG i)))
               (was (car pe))
               (connected (not (= 0 (bitwise-and p PORTSC-CURCONNECT)))))
          (if (not (= 0 (bitwise-and p PORTSC-CONNECTCHG)))
              (u-w16 iobar (PORTSC-REG i) PORTSC-CONNECTCHG))   ; ack change
          (cond
            ((and connected (not was))
             (display "[uhci] device connected, port ") (display i) (newline)
             (enable-port! iobar i)
             (let* ((after (u-r16 iobar (PORTSC-REG i)))
                    (spd (if (not (= 0 (bitwise-and after PORTSC-LOWSPEED)))
                             USB-SPEED-LOW USB-SPEED-FULL)))
               (send usb (list 'port-connected (self) i spd)))
             (loop (+ i 1) (cdr pe) (cons #t acc)))
            ((and (not connected) was)
             (display "[uhci] device disconnected, port ") (display i) (newline)
             (send usb (list 'port-disconnected (self) i))
             (loop (+ i 1) (cdr pe) (cons #f acc)))
            (else (loop (+ i 1) (cdr pe) (cons was acc))))))))

(define (uhci-clear-toggle ep-toggle addr)
  (let ((base (* (bitwise-and addr #x7F) 16)))
    (let loop ((i 0))
      (if (< i 16) (begin (bytes-u8-set! ep-toggle (+ base i) 0) (loop (+ i 1))) 'done))))

;; Service one transfer-request message. interrupt-in/bulk track the data toggle
;; in ep-toggle; isoch schedules into the frame list (fl) via the iso TD/data
;; buffers; prepare-downstream/mark-hub are UHCI no-ops (reply success).
(define (uhci-handle iobar dma dma-phys fl itd itd-phys idata idata-phys ep-toggle m)
  (let ((tag (car m)))
    (cond
      ((eq? tag 'control)              ; (control addr speed mps setup data len reply)
       (let ((r (uhci-control iobar dma dma-phys (cadr m) (caddr m) (cadddr m)
                              (nth m 4) (nth m 5) (nth m 6))))
         (send (nth m 7) (list 'complete (car r) (cadr r)))))
      ((eq? tag 'interrupt-in)         ; (interrupt-in addr speed ep maxp len reply)
       (let* ((ls (if (= (caddr m) USB-SPEED-LOW) 1 0))
              (r (uhci-data iobar dma dma-phys ep-toggle (cadr m) ls
                            (cadddr m) #t (nth m 4) #f (nth m 5) MS-5)))
         (send (nth m 6) (list 'complete (car r) (cadr r)))))
      ((eq? tag 'bulk)                 ; (bulk addr ep maxp data len dir-in? reply)
       (let ((r (uhci-data iobar dma dma-phys ep-toggle (cadr m) 0
                           (caddr m) (nth m 6) (cadddr m) (nth m 4) (nth m 5) MS-200)))
         (send (nth m 7) (list 'complete (car r) (cadr r)))))
      ((eq? tag 'isoch)                ; (isoch addr speed ep maxp data len dir-in? reply)
       (let ((r (uhci-isoch iobar fl dma-phys itd itd-phys idata idata-phys
                            (nth m 1) (nth m 3) (nth m 7) (nth m 4) (nth m 5) (nth m 6))))
         (send (nth m 8) (list 'complete (car r) (cadr r)))))
      ((eq? tag 'prepare-downstream)   ; (... parent port speed reply) -- no-op
       (send (nth m 4) (list 'complete 0 #f)))
      ((eq? tag 'mark-hub)             ; (... addr nports reply) -- no-op
       (send (cadddr m) (list 'complete 0 #f)))
      ((eq? tag 'disconnect-dev)       ; (... addr)
       (uhci-clear-toggle ep-toggle (cadr m)))
      (else 'ignore))))

;; The host-controller context loop: drain pending transfer messages fast, then
;; poll the ports at POLL-INTERVAL, napping IDLE-NS when there is nothing to do.
;; `b` bundles the per-controller DMA buffers (dma/iso TD/iso data + frame list)
;; so the loop's signature stays readable: (fl dma dma-phys itd itd-phys idata
;; idata-phys ep-toggle).
(define (uhci-loop iobar b usb port-enum last-poll)
  (cond
    ((not (%mailbox-empty?))
     (uhci-handle iobar (nth b 1) (nth b 2) (nth b 0) (nth b 3) (nth b 4) (nth b 5) (nth b 6) (nth b 7) (%mailbox-pop))
     (uhci-loop iobar b usb port-enum last-poll))
    ((> (- (uptime-ns) last-poll) POLL-INTERVAL)
     (let ((pe (poll-ports! iobar usb port-enum)))
       (uhci-loop iobar b usb pe (uptime-ns))))
    (else (sleep IDLE-NS)
          (uhci-loop iobar b usb port-enum last-poll))))

;; Bring-up body (runs in the spawned HC context): allocate the frame list + DMA
;; scratch, point every frame at the idle control QH, reset, then serve.
(define (uhci-bringup iobar usb)
  (let* ((fl (dma-alloc-32 4096)) (fl-phys (bytes-phys fl))
         (dma (dma-alloc-32 4096)) (dma-phys (bytes-phys dma))
         (itd (dma-alloc-32 4096)) (itd-phys (bytes-phys itd))
         (idata (dma-alloc-32 ISO-DATA-MAX)) (idata-phys (bytes-phys idata))
         (ep-toggle (make-bytes (* 128 16))))
    (bytes-u32-set! dma UHCI-QH-OFF 1)             ; QH hlp = Terminate
    (bytes-u32-set! dma (+ UHCI-QH-OFF 4) 1)       ; QH elp = Terminate (idle)
    (let loop ((i 0))                              ; every frame -> control QH
      (if (< i FRAME-COUNT)
          (begin (bytes-u32-set! fl (* i 4) (bitwise-or dma-phys 2)) (loop (+ i 1)))
          'done))
    (uhci-reset iobar fl-phys)
    (display "[uhci] reset complete; serving") (newline)
    (uhci-loop iobar (list fl dma dma-phys itd itd-phys idata idata-phys ep-toggle)
               usb (list #f #f) 0)))

;; Entry point (init.clp calls this with the coreusb handle). Discovers the
;; controller, enables it, and spawns the HC context. Gated on pci-find, so a
;; boot with no UHCI just logs and returns.
(define (uhci-init usb)
  (let ((ecam (find-uhci UHCI-IDS)))
    (if (not ecam)
        (begin (display "[uhci] no controller present") (newline) #f)
        (let ((cfg (mmio-map ecam 4096)))
          (pci-enable-mem-bus-master! cfg)
          (let ((iobar (bar-base cfg 4)))          ; UHCI registers are at the I/O BAR (BAR4)
            (display "[uhci] iobar=") (display iobar) (newline)
            (spawn-restricted '() (lambda () (uhci-bringup iobar usb)))
            'uhci-spawned)))))
