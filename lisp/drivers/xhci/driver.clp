;; xhci/driver -- the xHCI host controller: init/reset, the slot/address model,
;; command + event rings, control/interrupt/bulk transfers, hub support, and the
;; HC context loop. Ported from drivers/xhci/src/main.c.
;;
;; All mutable state (ring cursors, the event-ring dequeue, the slot table, the
;; per-endpoint rings, the address->slot map) lives in set!-able bindings of the
;; bring-up thunk, since a single transfer mutates rings mid-handler -- it cannot
;; be threaded through the message loop's return. The HC is one context, so this
;; mutation is race-free. As with the C driver, per-command input contexts and
;; per-endpoint rings are allocated and not freed except on disconnect; that is a
;; bounded leak across enumerations (no dma-free primitive), noted like ahci's.
;;
;; The xHCI addressing adapter (matching the C): on a port connect we Enable Slot
;; + Address Device(BSR=1) so EP0 works at the default address, then drive normal
;; coreusb enumeration; the control handler intercepts SET_ADDRESS and issues
;; Address Device(BSR=0), recording the coreusb address->slot mapping.

;; qemu-xhci (1b36:000d), nec-usb-xhci (1033:0194), and a couple Intel PCH xHCIs.
(define XHCI-IDS (list (list #x1b36 #x000d) (list #x1033 #x0194)
                       (list #x8086 #x9d2f) (list #x8086 #xa12f) (list #x8086 #x31a8)))
(define (find-xhci ids)
  (if (null? ids) #f
      (let ((e (pci-find (caar ids) (cadr (car ids))))) (if e e (find-xhci (cdr ids))))))

(define XPOLL-INTERVAL 250000000)
(define XIDLE-NS         2000000)

(define (speed-mps0 xspeed) (cond ((= xspeed 4) 512) ((= xspeed 3) 64) (else 8)))
(define (xspeed->uspeed x) (cond ((= x 2) USB-SPEED-LOW) ((= x 3) USB-SPEED-HIGH)
                                 ((= x 4) USB-SPEED-SUPER) (else USB-SPEED-FULL)))

;; slot record: (slot-id dev-ctx-buf dev-ctx-phys speed root-port route depth)
(define (mk-slot id dc dcp speed rp route depth) (list id dc dcp speed rp route depth))
(define (sl-id s) (car s))   (define (sl-devctx s) (cadr s))
(define (sl-speed s) (cadddr s)) (define (sl-rootport s) (nth s 4))
(define (sl-route s) (nth s 5))  (define (sl-depth s) (nth s 6))

(define (xhci-init usb)
  (let ((ecam (find-xhci XHCI-IDS)))
    (if (not ecam)
        (begin (display "[xhci] no controller present") (newline) #f)
        (let ((cfg (mmio-map ecam 4096)))
          (pci-enable-mem-bus-master! cfg)
          (let ((bar (let ((b (bar-base cfg 0)))
                       (if (= b 0) (begin (pci-assign-bars ecam) (bar-base cfg 0)) b))))
            (if (or (not bar) (= bar 0))
                (begin (display "[xhci] no BAR") (newline) #f)
                (begin (display "[xhci] bar=") (display bar) (newline)
                       (spawn-restricted '() (lambda () (xhci-bringup bar ecam usb)))
                       'xhci-spawned)))))))

;; The HC context: maps MMIO, resets, builds the rings/contexts, sets up MSI, runs
;; the controller, powers the ports, then serves transfer messages + polls ports.
(define (xhci-bringup bar ecam usb)
  (let* ((mmio (mmio-map bar 65536))
         (caplen (bytes-u8-ref mmio XHCI-CAP-CAPLENGTH))
         (op caplen)
         (rt (bitwise-and (rd32 mmio XHCI-CAP-RTSOFF) (bitwise-not #x1F)))
         (db (bitwise-and (rd32 mmio XHCI-CAP-DBOFF) (bitwise-not 3)))
         (hcs1 (rd32 mmio XHCI-CAP-HCSPARAMS1))
         (max-slots (bitwise-and hcs1 #xFF))
         (max-ports (bitwise-and (arithmetic-shift hcs1 -24) #xFF))
         (hcc1 (rd32 mmio XHCI-CAP-HCCPARAMS1))
         (ctx-size (if (not (= 0 (bitwise-and hcc1 4))) 64 32))
         (dcbaa (dma-alloc-32 4096))
         (cmd-ring (ring-make))
         (event-buf (dma-alloc-32 4096)) (event-phys 0)
         (erst (dma-alloc-32 4096))
         (bounce (dma-alloc-32 4096)) (bounce-phys 0)
         (ev-idx 0) (ev-cyc 1)
         (slots '()) (slot-eps '())
         (addr-to-slot (make-bytes 256))
         (enum-slot 0) (msi #f))
    (set! event-phys (bytes-phys event-buf))
    (set! bounce-phys (bytes-phys bounce))

    ;; --- slot + endpoint-ring registries (set!-able alists) ---
    (define (get-slot id) (let ((p (assq-num id slots))) (if p (cdr p) #f)))
    (define (add-slot! id sl) (set! slots (cons (cons id sl) slots)))
    (define (del-slot! id) (set! slots (filter (lambda (p) (not (= (car p) id))) slots)))
    (define (ep-key id dci) (+ (* id 64) dci))
    (define (get-ep-ring id dci) (let ((p (assq-num (ep-key id dci) slot-eps))) (if p (cdr p) #f)))
    (define (set-ep-ring! id dci r) (set! slot-eps (cons (cons (ep-key id dci) r) slot-eps)))
    (define (del-eps! id)
      (set! slot-eps (filter (lambda (p) (not (= (quotient (car p) 64) id))) slot-eps)))
    (define (db-ring! id dci) (wr32 mmio (+ db (* id 4)) dci))

    ;; --- event ring: pop next event (cycle-matched), advancing ERDP. Returns
    ;; (param status control) or #f. ---
    (define (next-event)
      (let* ((o (* ev-idx 16)) (ctrl (rd32 event-buf (+ o 12))))
        (if (not (= (bitwise-and ctrl 1) ev-cyc))
            #f
            (let ((param (rd64 event-buf o)) (status (rd32 event-buf (+ o 8))))
              (set! ev-idx (+ ev-idx 1))
              (if (= ev-idx XHCI-RING-SIZE) (begin (set! ev-idx 0) (set! ev-cyc (if (= ev-cyc 1) 0 1))))
              (wr64 mmio (+ rt XHCI-RT-IR0 XHCI-IR-ERDP) (bitwise-or (+ event-phys (* ev-idx 16)) 8))
              (list param status ctrl)))))

    ;; Wait for a command/transfer completion by POLLING the event ring (the
    ;; authoritative completion source -- the controller DMAs events to memory),
    ;; yielding via sleep between polls; we do not park on the MSI (the poll never
    ;; misses a completion). The deadline is checked at the top of every iteration
    ;; so a burst of intervening events (e.g. port-status) cannot livelock the wait.
    (define (xhci-wait type slot timeout-ns)
      (let ((deadline (+ (uptime-ns) timeout-ns)))
        (let loop ()
          (if (> (uptime-ns) deadline)
              #f
              (let ((ev (next-event)))
                (cond
                  ((not ev) (sleep 100000) (loop))
                  ((and (= (trb-type (caddr ev)) type)
                        (or (< slot 0) (= (trb-slot (caddr ev)) slot))) ev)
                  (else (loop))))))))   ; consume non-matching (port-status etc.)

    (define (xhci-command param control)
      (ring-push cmd-ring param 0 control)
      (db-ring! 0 0)
      (xhci-wait TRB-EVENT-CMD-COMPLETE -1 400000000))
    (define (cmd-ok? ev) (and ev (= (trb-cc (cadr ev)) XHCI-CC-SUCCESS)))

    ;; Address Device (bsr=1 sets up EP0 without sending SET_ADDRESS).
    (define (address-device sl bsr)
      (let ((input (dma-alloc-32 4096)) (ring (get-ep-ring (sl-id sl) 1)))
        (let ((in-phys (bytes-phys input)) (sc ctx-size) (ec (* ctx-size 2)))
          (bytes-u32-set! input 4 (bitwise-or 1 2))                  ; ICC add: slot + ep0
          (bytes-u32-set! input (+ sc 0) (bitwise-or (bitwise-and (sl-route sl) #xFFFFF)
                                                     (arithmetic-shift (sl-speed sl) 20)
                                                     (arithmetic-shift 1 27)))   ; ctx entries = 1
          (bytes-u32-set! input (+ sc 4) (arithmetic-shift (sl-rootport sl) 16))
          (bytes-u32-set! input (+ ec 4) (bitwise-or (arithmetic-shift EP-TYPE-CONTROL 3)
                                                     (arithmetic-shift 3 1)
                                                     (arithmetic-shift (speed-mps0 (sl-speed sl)) 16)))
          ;; TR Dequeue Pointer = the ring's CURRENT enqueue position + producer
          ;; cycle, not the ring base. On the second Address Device (BSR=0, after
          ;; the BSR=1 phase already pushed the initial GET_DESCRIPTOR TRBs) this
          ;; avoids re-pointing the controller at the already-consumed stale TRBs
          ;; (which it would otherwise re-execute). For a fresh ring (enq=0,cyc=1)
          ;; this is exactly ring-base|1, so the BSR=1 path is unchanged.
          (let ((trdp (bitwise-or (+ (ring-phys ring) (* (ring-enq ring) 16)) (ring-cyc ring))))
            (bytes-u32-set! input (+ ec 8) (bitwise-and trdp #xFFFFFFFF))
            (bytes-u32-set! input (+ ec 12) (arithmetic-shift trdp -32)))
          (bytes-u32-set! input (+ ec 16) 8)
          (if (cmd-ok? (xhci-command in-phys
                        (bitwise-or (trb-set-type TRB-ADDRESS-DEVICE)
                                    (if (= bsr 1) (arithmetic-shift 1 9) 0)
                                    (arithmetic-shift (sl-id sl) 24))))
              0 -1))))

    ;; Configure a non-control endpoint (once per slot+dci): build its ring and
    ;; issue Configure Endpoint.
    (define (configure-endpoint sl dci ep-type mps)
      (if (get-ep-ring (sl-id sl) dci)
          0
          (let ((ring (ring-make)) (input (dma-alloc-32 4096)))
            (set-ep-ring! (sl-id sl) dci ring)
            (let ((in-phys (bytes-phys input)) (sc ctx-size) (ec (* ctx-size (+ 1 dci))))
              (bytes-u32-set! input 4 (bitwise-or 1 (arithmetic-shift 1 dci)))
              (let ((dc0 (bytes-u32-ref (sl-devctx sl) 0)))
                (bytes-u32-set! input (+ sc 0)
                  (bitwise-or (bitwise-and dc0 (bitwise-not (arithmetic-shift #x1F 27)))
                              (arithmetic-shift dci 27))))
              (bytes-u32-set! input (+ sc 4) (arithmetic-shift (sl-rootport sl) 16))
              (bytes-u32-set! input (+ ec 4) (bitwise-or (arithmetic-shift ep-type 3)
                                                         (arithmetic-shift 3 1)
                                                         (arithmetic-shift mps 16)))
              (let ((trdp (bitwise-or (ring-phys ring) 1)))
                (bytes-u32-set! input (+ ec 8) (bitwise-and trdp #xFFFFFFFF))
                (bytes-u32-set! input (+ ec 12) (arithmetic-shift trdp -32)))
              (bytes-u32-set! input (+ ec 16) (bitwise-and mps #xFFFF))
              (if (cmd-ok? (xhci-command in-phys
                            (bitwise-or (trb-set-type TRB-CONFIGURE-ENDPOINT)
                                        (arithmetic-shift (sl-id sl) 24))))
                  0 -1)))))

    ;; Control transfer on EP0. Returns (list n data) like the UHCI engine.
    (define (do-control sl setup data data-len)
      (let* ((ring (get-ep-ring (sl-id sl) 1))
             (is-read (not (= 0 (bitwise-and (bytes-u8-ref setup 0) USB-REQ-DIR-IN))))
             (dlen (if (> data-len XHCI-BOUNCE-MAX) XHCI-BOUNCE-MAX data-len))
             (trt (if (= dlen 0) 0 (if is-read 3 2))))
        (ring-push ring (bytes-u64-ref setup 0) 8
                   (bitwise-or (trb-set-type TRB-SETUP) (arithmetic-shift 1 6)
                               (arithmetic-shift trt 16)))
        (if (> dlen 0)
            (begin
              (if (and (not is-read) data) (bytes-copy-into! bounce 0 data dlen))
              (ring-push ring bounce-phys dlen
                         (bitwise-or (trb-set-type TRB-DATA) (if is-read (arithmetic-shift 1 16) 0)))))
        (ring-push ring 0 0 (bitwise-or (trb-set-type TRB-STATUS)
                                        (if (and (not is-read) (> dlen 0)) (arithmetic-shift 1 16) 0)
                                        (arithmetic-shift 1 5)))   ; IOC
        (db-ring! (sl-id sl) 1)
        (let ((ev (xhci-wait TRB-EVENT-TRANSFER (sl-id sl) 400000000)))
          (if (not ev)
              (list -1 #f)
              (let ((cc (trb-cc (cadr ev))))
                (if (and (not (= cc XHCI-CC-SUCCESS)) (not (= cc XHCI-CC-SHORT-PACKET)))
                    (list -1 #f)
                    (if (and is-read (> dlen 0))
                        (list dlen (copy-bytes bounce 0 dlen))
                        (list dlen #f))))))))

    ;; Single-TRB interrupt/bulk transfer. Returns (list n data).
    (define (do-data sl dci ep-type mps data len dir-in? timeout)
      (if (not (= (configure-endpoint sl dci ep-type mps) 0))
          (list -1 #f)
          (let* ((ring (get-ep-ring (sl-id sl) dci))
                 (dlen (if (> len XHCI-BOUNCE-MAX) XHCI-BOUNCE-MAX len)))
            (if (and (not dir-in?) data) (bytes-copy-into! bounce 0 data dlen))
            (ring-push ring bounce-phys dlen
                       (bitwise-or (trb-set-type TRB-NORMAL) (arithmetic-shift 1 5)
                                   (arithmetic-shift 1 2)))   ; IOC | ISP
            (db-ring! (sl-id sl) dci)
            (let ((ev (xhci-wait TRB-EVENT-TRANSFER (sl-id sl) timeout)))
              (if (not ev)
                  (if dir-in? (list 0 (make-bytes 0)) (list -1 #f))
                  (let ((cc (trb-cc (cadr ev))))
                    (if (and (not (= cc XHCI-CC-SUCCESS)) (not (= cc XHCI-CC-SHORT-PACKET)))
                        (if dir-in? (list 0 (make-bytes 0)) (list -1 #f))
                        (let* ((residual (bitwise-and (cadr ev) #xFFFFFF))
                               (got (let ((g (- dlen residual))) (if (< g 0) 0 g))))
                          (if dir-in? (list got (copy-bytes bounce 0 got)) (list got #f))))))))))

    ;; Hub: enable + address a downstream device, building the route string from
    ;; the parent hub's slot. Returns 0/-1.
    (define (prepare-downstream parent-addr parent-port speed)
      (let ((psl (get-slot (bytes-u8-ref addr-to-slot (bitwise-and parent-addr #xFF)))))
        (if (not psl) -1
            (let ((ev (xhci-command 0 (trb-set-type TRB-ENABLE-SLOT))))
              (if (not (cmd-ok? ev)) -1
                  (let ((slot-id (trb-slot (caddr ev))))
                    (if (or (<= slot-id 0) (> slot-id max-slots)) -1
                        (let* ((devctx (dma-alloc-32 4096)) (ring (ring-make))
                               (route (bitwise-or (sl-route psl)
                                                  (arithmetic-shift (bitwise-and parent-port #xF)
                                                                    (* 4 (sl-depth psl)))))
                               (sl (mk-slot slot-id devctx (bytes-phys devctx)
                                            (xspeed-of speed) (sl-rootport psl) route (+ (sl-depth psl) 1))))
                          (add-slot! slot-id sl)
                          (set-ep-ring! slot-id 1 ring)
                          (bytes-u64-set! dcbaa (* slot-id 8) (bytes-phys devctx))
                          (if (= (address-device sl 1) 0)
                              (begin (set! enum-slot slot-id) 0)
                              -1)))))))))
    (define (xspeed-of uspeed) (cond ((= uspeed USB-SPEED-LOW) 2) ((= uspeed USB-SPEED-HIGH) 3)
                                     ((= uspeed USB-SPEED-SUPER) 4) (else 1)))

    ;; Mark a slot as a hub (Hub bit + nports) via Configure Endpoint on the slot
    ;; context only. Returns 0/-1.
    (define (mark-hub-dev addr nports)
      (let ((sl (get-slot (bytes-u8-ref addr-to-slot (bitwise-and addr #xFF)))))
        (if (not sl) -1
            (let ((input (dma-alloc-32 4096)) (dc (sl-devctx sl)))
              (let ((in-phys (bytes-phys input)) (sc ctx-size))
                (bytes-u32-set! input 4 1)                      ; ICC add: slot only
                (bytes-u32-set! input (+ sc 0) (bitwise-or (bytes-u32-ref dc 0) (arithmetic-shift 1 26)))
                (bytes-u32-set! input (+ sc 4) (bitwise-or (bitwise-and (bytes-u32-ref dc 4) #x00FFFFFF)
                                                           (arithmetic-shift (bitwise-and nports #xFF) 24)))
                (bytes-u32-set! input (+ sc 8) (bytes-u32-ref dc 8))
                (bytes-u32-set! input (+ sc 12) (bytes-u32-ref dc 12))
                (if (cmd-ok? (xhci-command in-phys (bitwise-or (trb-set-type TRB-CONFIGURE-ENDPOINT)
                                                               (arithmetic-shift (sl-id sl) 24))))
                    0 -1))))))

    (define (disconnect-dev addr)
      (let* ((a (bitwise-and addr #xFF)) (slot-id (bytes-u8-ref addr-to-slot a)))
        (if (and (> slot-id 0) (get-slot slot-id))
            (begin (xhci-command 0 (bitwise-or (trb-set-type TRB-DISABLE-SLOT) (arithmetic-shift slot-id 24)))
                   (bytes-u64-set! dcbaa (* slot-id 8) 0)
                   (del-eps! slot-id) (del-slot! slot-id)
                   (display "[xhci] slot disabled") (newline)))
        (bytes-u8-set! addr-to-slot a 0)))

    ;; Root-port connect: reset, enable slot, address device (BSR=1), then tell
    ;; coreusb (with this context as the controller handle) to enumerate.
    (define (port-connected p)
      (let ((psc (rd32 mmio (+ op (XHCI-OP-PORTSC p)))))
        (wr32 mmio (+ op (XHCI-OP-PORTSC p)) (bitwise-or (bitwise-and psc (bitwise-not XHCI-PORTSC-PED)) XHCI-PORTSC-PR))
        (wait-until (lambda () (not (= 0 (bitwise-and (rd32 mmio (+ op (XHCI-OP-PORTSC p))) XHCI-PORTSC-PRC)))) 100000000)
        (wr32 mmio (+ op (XHCI-OP-PORTSC p))
              (bitwise-or (rd32 mmio (+ op (XHCI-OP-PORTSC p))) XHCI-PORTSC-PRC XHCI-PORTSC-CSC))
        (if (= 0 (bitwise-and (rd32 mmio (+ op (XHCI-OP-PORTSC p))) XHCI-PORTSC-PED))
            (begin (display "[xhci] port not enabled after reset") (newline))
            (let ((xspeed (bitwise-and (arithmetic-shift (rd32 mmio (+ op (XHCI-OP-PORTSC p)))
                                                         (- XHCI-PORTSC-SPEED-SHIFT)) XHCI-PORTSC-SPEED-MASK))
                  (ev (xhci-command 0 (trb-set-type TRB-ENABLE-SLOT))))
              (if (not (cmd-ok? ev))
                  (begin (display "[xhci] enable slot failed") (newline))
                  (let ((slot-id (trb-slot (caddr ev))))
                    (if (or (<= slot-id 0) (> slot-id max-slots))
                        (begin (display "[xhci] bad slot id") (newline))
                        (let* ((devctx (dma-alloc-32 4096)) (ring (ring-make))
                               (sl (mk-slot slot-id devctx (bytes-phys devctx) xspeed p 0 0)))
                          (add-slot! slot-id sl)
                          (set-ep-ring! slot-id 1 ring)
                          (bytes-u64-set! dcbaa (* slot-id 8) (bytes-phys devctx))
                          (if (= (address-device sl 1) 0)
                              (begin (set! enum-slot slot-id)
                                     (display "[xhci] port up; enumerating") (newline)
                                     (send usb (list 'port-connected (self) p (xspeed->uspeed xspeed))))
                              (begin (display "[xhci] address device failed") (newline)))))))))))

    (define (poll-ports! seen)
      (let loop ((p 1))
        (if (<= p max-ports)
            (let* ((psc (rd32 mmio (+ op (XHCI-OP-PORTSC p))))
                   (conn (not (= 0 (bitwise-and psc XHCI-PORTSC-CCS))))
                   (was (not (= 0 (bytes-u8-ref seen p)))))
              (cond ((and conn (not was)) (bytes-u8-set! seen p 1) (port-connected p))
                    ((and (not conn) was) (bytes-u8-set! seen p 0)
                                          (display "[xhci] port down") (newline)
                                          (send usb (list 'port-disconnected (self) p))))
              (loop (+ p 1))))))

    ;; transfer-request message dispatch (same protocol as UHCI).
    (define (handle m)
      (let ((tag (car m)))
        (cond
          ((eq? tag 'control)        ; (control addr speed mps setup data len reply)
           (let* ((addr (cadr m)) (setup (nth m 4)) (data (nth m 5)) (len (nth m 6)) (reply (nth m 7))
                  (slot-id (if (= addr 0) enum-slot (bytes-u8-ref addr-to-slot (bitwise-and addr #xFF))))
                  (sl (if (> slot-id 0) (get-slot slot-id) #f)))
             (if (not sl) (send reply (list 'complete -1 #f))
                 (if (and (= (bytes-u8-ref setup 0) 0) (= (bytes-u8-ref setup 1) USB-REQ-SET-ADDRESS))
                     (if (= (address-device sl 0) 0)
                         (begin (bytes-u8-set! addr-to-slot (bitwise-and (bytes-u16-ref setup 2) #xFF) slot-id)
                                (send reply (list 'complete 0 #f)))
                         (send reply (list 'complete -1 #f)))
                     (let ((r (do-control sl setup data len)))
                       (send reply (list 'complete (car r) (cadr r))))))))
          ((eq? tag 'interrupt-in)   ; (interrupt-in addr speed ep maxp len reply)
           (let ((slot-id (bytes-u8-ref addr-to-slot (bitwise-and (cadr m) #xFF))))
             (if (<= slot-id 0) (send (nth m 6) (list 'complete -1 #f))
                 (let ((r (do-data (get-slot slot-id) (+ (* (bitwise-and (cadddr m) #xF) 2) 1)
                                   EP-TYPE-INTR-IN (nth m 4) #f (nth m 5) #t 8000000)))
                   (send (nth m 6) (list 'complete (car r) (cadr r)))))))
          ((eq? tag 'bulk)           ; (bulk addr ep maxp data len dir-in? reply)
           (let ((slot-id (bytes-u8-ref addr-to-slot (bitwise-and (cadr m) #xFF))) (dir (nth m 6)))
             (if (<= slot-id 0) (send (nth m 7) (list 'complete -1 #f))
                 (let ((r (do-data (get-slot slot-id)
                                   (+ (* (bitwise-and (caddr m) #xF) 2) (if dir 1 0))
                                   (if dir EP-TYPE-BULK-IN EP-TYPE-BULK-OUT)
                                   (cadddr m) (nth m 4) (nth m 5) dir 300000000)))
                   (send (nth m 7) (list 'complete (car r) (cadr r)))))))
          ((eq? tag 'prepare-downstream)   ; (... parent-addr port speed reply)
           (send (nth m 4) (list 'complete (prepare-downstream (cadr m) (caddr m) (cadddr m)) #f)))
          ((eq? tag 'mark-hub)             ; (... addr nports reply)
           (send (cadddr m) (list 'complete (mark-hub-dev (cadr m) (caddr m)) #f)))
          ((eq? tag 'disconnect-dev) (disconnect-dev (cadr m)))
          (else 'ignore))))

    ;; --- controller init ---
    (wr32 mmio (+ op XHCI-OP-USBCMD) 0)
    (wait-until (lambda () (not (= 0 (bitwise-and (rd32 mmio (+ op XHCI-OP-USBSTS)) XHCI-USBSTS-HCH)))) 100000000)
    (wr32 mmio (+ op XHCI-OP-USBCMD) XHCI-USBCMD-HCRST)
    (wait-until (lambda () (and (= 0 (bitwise-and (rd32 mmio (+ op XHCI-OP-USBCMD)) XHCI-USBCMD-HCRST))
                                (= 0 (bitwise-and (rd32 mmio (+ op XHCI-OP-USBSTS)) XHCI-USBSTS-CNR))))
                1000000000)
    (wr32 mmio (+ op XHCI-OP-CONFIG) (if (> max-slots 64) 64 max-slots))
    (wr64 mmio (+ op XHCI-OP-DCBAAP) (bytes-phys dcbaa))
    (wr64 mmio (+ op XHCI-OP-CRCR) (bitwise-or (ring-phys cmd-ring) 1))
    ;; event ring + ERST (single segment)
    (bytes-u64-set! erst 0 event-phys)
    (bytes-u32-set! erst 8 XHCI-RING-SIZE)
    (wr32 mmio (+ rt XHCI-RT-IR0 XHCI-IR-ERSTSZ) 1)
    (wr64 mmio (+ rt XHCI-RT-IR0 XHCI-IR-ERDP) event-phys)
    (wr64 mmio (+ rt XHCI-RT-IR0 XHCI-IR-ERSTBA) (bytes-phys erst))
    (set! msi (pci-setup-msi ecam))
    (wr32 mmio (+ rt XHCI-RT-IR0 XHCI-IR-IMOD) 4000)
    (wr32 mmio (+ rt XHCI-RT-IR0 XHCI-IR-IMAN) XHCI-IMAN-IE)
    (wr32 mmio (+ op XHCI-OP-USBCMD) (bitwise-or XHCI-USBCMD-RS XHCI-USBCMD-INTE))
    ;; power all ports
    (let pp ((p 1)) (if (<= p max-ports)
                        (begin (wr32 mmio (+ op (XHCI-OP-PORTSC p))
                                     (bitwise-or (rd32 mmio (+ op (XHCI-OP-PORTSC p))) XHCI-PORTSC-PP))
                               (pp (+ p 1)))))
    (sleep 100000000)
    (display "[xhci] init complete; slots=") (display max-slots)
    (display " ports=") (display max-ports) (display " ctxsize=") (display ctx-size)
    (display " msi=") (display msi) (newline)

    ;; serve transfers + poll ports
    (let ((seen (make-bytes (+ max-ports 1))))
      (let loop ((last-poll 0))
        (cond
          ((not (%mailbox-empty?)) (handle (%mailbox-pop)) (loop last-poll))
          ((> (- (uptime-ns) last-poll) XPOLL-INTERVAL) (poll-ports! seen) (loop (uptime-ns)))
          (else (sleep XIDLE-NS) (loop last-poll)))))))

;; small helper: assoc on a numeric key (eq? works on fixnums here, but use = for
;; clarity / safety across the fixnum range).
(define (assq-num k lst)
  (cond ((null? lst) #f) ((= (caar lst) k) (car lst)) (else (assq-num k (cdr lst)))))
