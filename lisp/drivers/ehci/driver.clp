;; ehci/driver -- the EHCI host controller: discovery/reset, the async-schedule
;; transfer engine (control/bulk/interrupt-in over one reusable QH), root-port
;; reset/scan, and the HC context loop. Ported in spirit from the uhci driver --
;; one CONTEXT both serves transfer messages and polls the ports, yielding via
;; sleep, with completion read straight from DMA-written qTD status (no MSI).
;;
;; EHCI root ports carry only HIGH speed. A full/low-speed device is handed to a
;; companion controller (UHCI/OHCI) by setting the port's Owner bit; if there is no
;; companion (a bare `usb-ehci`) that device simply won't bind here. So the natural
;; EHCI test device is a HIGH-speed one (usb-storage). Split transactions (FS/LS
;; behind a HS hub via a TT) and the periodic schedule (hardware-paced interrupt/
;; iso) are not implemented -- noted in AUDIT.

;; QEMU: usb-ehci (8086:24cd), ich9-usb-ehci1/2 (8086:293a/293c), plus a NEC id.
(define EHCI-IDS (list (list #x8086 #x24cd) (list #x8086 #x293a) (list #x8086 #x293c)
                       (list #x1033 #x00e0)))
(define (find-ehci ids)
  (if (null? ids) #f
      (let ((e (pci-find (caar ids) (cadr (car ids))))) (if e e (find-ehci (cdr ids))))))

(define EPOLL-INTERVAL 250000000)   ; 250ms root-port poll cadence
(define EIDLE-NS         2000000)   ; 2ms idle nap
(define EMS-50  50000000)
(define EMS-5    5000000)
(define EMS-200 200000000)

(define (n-falses n) (if (<= n 0) '() (cons #f (n-falses (- n 1)))))

;; Map a USB speed to the EHCI endpoint-speed field (high=2, full=1, low=0).
(define (speed->eps speed)
  (cond ((= speed USB-SPEED-HIGH) 2) ((= speed USB-SPEED-LOW) 0) (else 1)))

;; Stop, reset, then configure for async-only operation: 32-bit addressing, async
;; list head = our QH, no interrupts (we poll), run + async-enable, and route all
;; ports to EHCI (CONFIGFLAG). Bounded yields throughout.
(define (ehci-reset mmio op qh-phys)
  (e-w32 mmio (+ op EOP-USBCMD) 0)
  (wait-until (lambda () (not (= 0 (bitwise-and (e-r32 mmio (+ op EOP-USBSTS)) USBSTS-HCHALTED)))) EMS-50)
  (e-w32 mmio (+ op EOP-USBCMD) USBCMD-HCRESET)
  (wait-until (lambda () (= 0 (bitwise-and (e-r32 mmio (+ op EOP-USBCMD)) USBCMD-HCRESET))) 100000000)
  (e-w32 mmio (+ op EOP-CTRLDSSEG) 0)
  (e-w32 mmio (+ op EOP-ASYNCBASE) qh-phys)
  (e-w32 mmio (+ op EOP-USBINTR) 0)
  (e-w32 mmio (+ op EOP-USBCMD) (bitwise-or USBCMD-RS USBCMD-ASE USBCMD-ITC-1))
  (e-w32 mmio (+ op EOP-CONFIGFLAG) 1)
  (wait-until (lambda () (not (= 0 (bitwise-and (e-r32 mmio (+ op EOP-USBSTS)) USBSTS-ASS)))) EMS-50))

;; Poll the qTD at index `last` until it retires (Active clears), the QH overlay
;; latches Halted (error), or timeout. Yields between polls.
(define (ehci-poll mmio sched last timeout-ns)
  (let ((deadline (+ (uptime-ns) timeout-ns)))
    (let loop ()
      (cond ((qh-overlay-halted? sched) 'halt)
            ((not (qtd-active? sched last)) 'done)
            ((> (uptime-ns) deadline) 'timeout)
            (else (sleep 100000) (loop))))))

;; Control transfer on EP0: SETUP + optional DATA + STATUS qTDs, armed on the QH,
;; polled to the STATUS stage. Returns (list n data) like the uhci engine.
(define (ehci-control mmio op sched sched-phys data data-phys addr speed mps setup payload len)
  (let* ((mps (if (<= mps 0) 8 mps))
         (len (cond ((< len 0) 0) ((> len EHCI-DATA-MAX) EHCI-DATA-MAX) (else len)))
         (eps (speed->eps speed))
         (is-read (not (= 0 (bitwise-and (bytes-u8-ref setup 0) USB-REQ-DIR-IN))))
         (has-data (> len 0))
         (setup-phys (+ sched-phys EHCI-SETUP-OFF))
         (qtd0 (+ sched-phys EHCI-QTD-OFF))
         (qtd1 (+ qtd0 32)) (qtd2 (+ qtd0 64))
         (data-pid   (if is-read EHCI-PID-IN EHCI-PID-OUT))
         (status-pid (if is-read EHCI-PID-OUT EHCI-PID-IN))
         (last (if has-data 2 1)))
    (bytes-copy-into! sched EHCI-SETUP-OFF setup 8)
    (if (and has-data (not is-read) payload) (bytes-copy-into! data 0 payload len))
    ;; SETUP always links to qTD index 1, which is the DATA stage (has-data) or the
    ;; STATUS stage (no-data) -- so the chain is SETUP->1->(2) with no dangling skip.
    (qtd! sched 0 qtd1 EHCI-PID-SETUP 8 0 setup-phys #f)                      ; SETUP, DATA0
    (if has-data
        (qtd! sched 1 qtd2 data-pid len 1 data-phys #f))                      ; DATA, DATA1
    (qtd! sched last 1 status-pid 0 1 0 #t)                                   ; STATUS, DATA1, IOC
    (qh-config! sched qtd0 addr 0 eps mps #t)
    (let ((r (ehci-poll mmio sched last EMS-200)))
      (qh-idle! sched)
      (if (not (eq? r 'done))
          (list -1 #f)
          (let ((total (if has-data (let ((g (- len (qtd-remaining sched 1)))) (if (< g 0) 0 g)) 0)))
            (if (and is-read (> total 0))
                (list total (copy-bytes data 0 (if (> total len) len total)))
                (list total #f)))))))

;; Single-qTD data transfer (bulk or interrupt-in), with the per-(addr,ep) toggle
;; tracked in ep-toggle. Returns (list n data). A timed-out IN (the endpoint only
;; NAK'd within budget) reports 0 bytes -- the interrupt-poll "no data now" answer.
(define (ehci-data mmio op sched sched-phys data data-phys ep-toggle addr speed endpoint dir-in? mps payload len timeout-ns)
  (let* ((mps (if (<= mps 0) 8 mps))
         (len (cond ((< len 0) 0) ((> len EHCI-DATA-MAX) EHCI-DATA-MAX) (else len)))
         (eps (speed->eps speed))
         (ep (bitwise-and endpoint #xF))
         (tidx (+ (* (bitwise-and addr #x7F) 16) ep))
         (toggle (bitwise-and (bytes-u8-ref ep-toggle tidx) 1))
         (pid (if dir-in? EHCI-PID-IN EHCI-PID-OUT))
         (qtd0 (+ sched-phys EHCI-QTD-OFF)))
    (if (and (not dir-in?) payload (> len 0)) (bytes-copy-into! data 0 payload len))
    (qtd! sched 0 1 pid len toggle data-phys #t)   ; single qTD, IOC
    (qh-config! sched qtd0 addr ep eps mps #f)
    (let ((r (ehci-poll mmio sched 0 timeout-ns)))
      (qh-idle! sched)
      (cond
        ((eq? r 'done)
         (let* ((rem (qtd-remaining sched 0))
                (total (let ((g (- len rem))) (if (< g 0) 0 g)))
                ;; the HC auto-toggled per packet within the qTD; advance our shadow
                ;; toggle by the number of packets actually transferred.
                (pkts (quotient (+ total (- mps 1)) mps)))
           (bytes-u8-set! ep-toggle tidx (bitwise-and (+ toggle pkts) 1))
           (if dir-in? (list total (copy-bytes data 0 (if (> total len) len total))) (list total #f))))
        ((and (eq? r 'timeout) dir-in?) (list 0 (make-bytes 0)))
        (else (list -1 #f))))))

(define (ehci-clear-toggle ep-toggle addr)
  (let ((base (* (bitwise-and addr #x7F) 16)))
    (let loop ((i 0))
      (if (< i 16) (begin (bytes-u8-set! ep-toggle (+ base i) 0) (loop (+ i 1))) 'done))))

;; Reset + enable a root port. A K-state line (low-speed device) is released to the
;; companion before reset. After a HS reset, PED=1 means a high-speed device (we
;; keep it); PED=0 means full-speed -> release to the companion. Returns a symbol.
(define (ehci-enable-port! mmio op n)
  (let ((psc (e-r32 mmio (+ op (EOP-PORTSC n)))))
    (cond
      ((= 0 (bitwise-and psc PORTSC-CCS)) 'none)
      ((= 1 (bitwise-and (arithmetic-shift psc (- PORTSC-LINE-SHIFT)) 3))
       (e-w32 mmio (+ op (EOP-PORTSC n)) (bitwise-or (bitwise-and psc PORTSC-PRESERVE) PORTSC-PO))
       'released-ls)
      (else
       (e-w32 mmio (+ op (EOP-PORTSC n))
              (bitwise-or (bitwise-and psc (bitwise-not PORTSC-PED)) PORTSC-PR))   ; assert reset
       (sleep EMS-50)
       (e-w32 mmio (+ op (EOP-PORTSC n))
              (bitwise-and (e-r32 mmio (+ op (EOP-PORTSC n))) (bitwise-not PORTSC-PR)))  ; deassert
       (wait-until (lambda () (= 0 (bitwise-and (e-r32 mmio (+ op (EOP-PORTSC n))) PORTSC-PR))) EMS-50)
       (if (not (= 0 (bitwise-and (e-r32 mmio (+ op (EOP-PORTSC n))) PORTSC-PED)))
           'high
           (begin (e-w32 mmio (+ op (EOP-PORTSC n))
                         (bitwise-or (bitwise-and (e-r32 mmio (+ op (EOP-PORTSC n))) PORTSC-PRESERVE) PORTSC-PO))
                  'released-fs))))))

;; Scan each root port; ack connect-change, and on a new connect reset+enable and
;; tell coreusb (high-speed only); on a disconnect tell coreusb. Returns updated
;; per-port "enumerated?" state.
(define (ehci-poll-ports! mmio op nports usb port-enum)
  (let loop ((i 0) (pe port-enum) (acc '()))
    (if (= i nports)
        (reverse acc)
        (let* ((psc (e-r32 mmio (+ op (EOP-PORTSC i))))
               (was (car pe))
               (connected (not (= 0 (bitwise-and psc PORTSC-CCS)))))
          (if (not (= 0 (bitwise-and psc (bitwise-or PORTSC-CSC PORTSC-PEDC))))
              (e-w32 mmio (+ op (EOP-PORTSC i))
                     (bitwise-or (bitwise-and psc PORTSC-PRESERVE) PORTSC-CSC PORTSC-PEDC)))  ; W1C
          (cond
            ((and connected (not was))
             (lg "device connected, port " i)
             (let ((res (ehci-enable-port! mmio op i)))
               (if (eq? res 'high)
                   (begin (lg "port " i " high-speed; enumerating")
                          (send usb (list 'port-connected (self) i USB-SPEED-HIGH))
                          (loop (+ i 1) (cdr pe) (cons #t acc)))
                   (begin (lg "port " i " " res " (released/none)")
                          (loop (+ i 1) (cdr pe) (cons #f acc))))))
            ((and (not connected) was)
             (lg "device disconnected, port " i)
             (send usb (list 'port-disconnected (self) i))
             (loop (+ i 1) (cdr pe) (cons #f acc)))
            (else (loop (+ i 1) (cdr pe) (cons was acc))))))))

;; Service one transfer-request message. bulk is HS-only on EHCI (speed HIGH);
;; interrupt-in is serviced as a one-shot async IN with a short NAK budget. isoch
;; and the hub split helpers are unsupported here (reply error / success no-op).
(define (ehci-handle mmio op sched sched-phys data data-phys ep-toggle m)
  (let ((tag (car m)))
    (cond
      ((eq? tag 'control)              ; (control addr speed mps setup data len reply)
       (let ((r (ehci-control mmio op sched sched-phys data data-phys
                              (cadr m) (caddr m) (cadddr m) (nth m 4) (nth m 5) (nth m 6))))
         (send (nth m 7) (list 'complete (car r) (cadr r)))))
      ((eq? tag 'interrupt-in)         ; (interrupt-in addr speed ep maxp len reply)
       (let ((r (ehci-data mmio op sched sched-phys data data-phys ep-toggle
                           (cadr m) (caddr m) (cadddr m) #t (nth m 4) #f (nth m 5) EMS-5)))
         (send (nth m 6) (list 'complete (car r) (cadr r)))))
      ((eq? tag 'bulk)                 ; (bulk addr ep maxp data len dir-in? reply)
       (let ((r (ehci-data mmio op sched sched-phys data data-phys ep-toggle
                           (cadr m) USB-SPEED-HIGH (caddr m) (nth m 6) (cadddr m) (nth m 4) (nth m 5) EMS-200)))
         (send (nth m 7) (list 'complete (car r) (cadr r)))))
      ((eq? tag 'isoch)                ; not supported on EHCI (no periodic schedule)
       (send (nth m 8) (list 'complete -1 #f)))
      ((eq? tag 'prepare-downstream)   ; no TT/split support -> no-op success
       (send (nth m 4) (list 'complete 0 #f)))
      ((eq? tag 'mark-hub)
       (send (cadddr m) (list 'complete 0 #f)))
      ((eq? tag 'disconnect-dev)
       (ehci-clear-toggle ep-toggle (cadr m)))
      (else 'ignore))))

;; The HC context loop: drain transfer messages fast, then poll ports at
;; EPOLL-INTERVAL, napping when idle. `b` bundles the DMA buffers
;; (sched sched-phys data data-phys ep-toggle).
(define (ehci-loop mmio op b nports usb port-enum last-poll)
  (cond
    ((not (%mailbox-empty?))
     (ehci-handle mmio op (nth b 0) (nth b 1) (nth b 2) (nth b 3) (nth b 4) (%mailbox-pop))
     (ehci-loop mmio op b nports usb port-enum last-poll))
    ((> (- (uptime-ns) last-poll) EPOLL-INTERVAL)
     (let ((pe (ehci-poll-ports! mmio op nports usb port-enum)))
       (ehci-loop mmio op b nports usb pe (uptime-ns))))
    (else (sleep EIDLE-NS) (ehci-loop mmio op b nports usb port-enum last-poll))))

;; Bring-up body (spawned context): map MMIO, allocate the schedule + data DMA,
;; init the async QH head, reset/configure, power the ports, then serve.
(define (ehci-bringup bar ecam usb)
  (let* ((mmio (mmio-map bar 4096))
         (op (bytes-u8-ref mmio ECAP-CAPLENGTH))
         (nports (bitwise-and (e-r32 mmio ECAP-HCSPARAMS) #xF))
         (sched (dma-alloc-32 4096)) (sched-phys (bytes-phys sched))
         (data (dma-alloc-32 8192)) (data-phys (bytes-phys data))
         (ep-toggle (make-bytes (* 128 16))))
    (qh-init-head! sched sched-phys)
    (ehci-reset mmio op sched-phys)
    (lg "reset complete; nports=" nports "; serving")
    (let pp ((i 0))
      (if (< i nports)
          (begin (e-w32 mmio (+ op (EOP-PORTSC i))
                        (bitwise-or (e-r32 mmio (+ op (EOP-PORTSC i))) PORTSC-PP))
                 (pp (+ i 1)))))
    (sleep EMS-50)
    (ehci-loop mmio op (list sched sched-phys data data-phys ep-toggle)
               nports usb (n-falses nports) 0)))

;; Entry point (init.clp calls this with the coreusb handle). Discovers the
;; controller, enables it, and spawns the HC context. pci-find-gated.
(define (ehci-init usb)
  (let ((ecam (find-ehci EHCI-IDS)))
    (if (not ecam)
        (begin (lg "no controller present") #f)
        (let ((cfg (mmio-map ecam 4096)))
          (pci-enable-mem-bus-master! cfg)
          (let ((bar (let ((b (bar-base cfg 0)))
                       (if (= b 0) (begin (pci-assign-bars ecam) (bar-base cfg 0)) b))))
            (if (or (not bar) (= bar 0))
                (begin (lg "no BAR") #f)
                (begin (lg "bar=" bar)
                       (spawn-restricted '() (lambda () (ehci-bringup bar ecam usb)))
                       'ehci-spawned)))))))
