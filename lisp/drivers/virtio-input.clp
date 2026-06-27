;; virtio-input: the virtio 1.0 input device (PCI 1af4:1052), in Cardinal Lisp.
;;
;; QEMU surfaces three flavours of this device -- -device virtio-tablet-pci
;; (absolute pointer), virtio-mouse-pci (relative pointer) and
;; virtio-keyboard-pci -- all on the same vendor/device id; the kind is told
;; apart by which event TYPES arrive (EV_ABS => tablet, EV_REL => mouse, EV_KEY
;; with keyboard keycodes => keyboard). This driver feeds coreinput the two
;; payload shapes the compositor consumes: (key <code> <pressed>) and
;; (pointer <x> <y> <down?>).
;;
;; The transport (PCI capability walk, status/feature handshake, split
;; virtqueue setup, the notify kick) is the shared `virtio` library; this module
;; is the input-SPECIFIC half. Only one feature is wanted: VIRTIO_F_VERSION_1.
;;
;; Two virtqueues exist: eventq (index 0, device->driver) and statusq (index 1,
;; driver->device, left idle). We fill the eventq with device-WRITABLE 8-byte
;; buffers; the device writes one virtio_input_event = {u16 type; u16 code;
;; u32 value} per slot and signals via the used ring (MSI-X). On each EV_SYN we
;; flush the accumulated pointer/key state to coreinput, then re-post the drained
;; buffers.
;;
;; The driver imports exactly the capabilities it needs -- sys-mmio (mmio-map/
;; dma-alloc) and sys-pci (pci-setup-msi + the MSI wake bridge msi-count/
;; msi-wait) -- plus driver-util and the shared virtio transport. It exports the
;; entry point virtio-input-init and the PURE event-parse + reducer helpers (so
;; the decode logic is host-testable without hardware).

(define-module virtio-input
  (export virtio-input-init
          ;; pure, host-testable decode core:
          parse-input-event pstate0 mk reduce-event reduce-events ps-out
          EV-SYN EV-KEY EV-REL EV-ABS REL-X REL-Y ABS-X ABS-Y BTN-LEFT)
  (import sys-mmio sys-pci driver-util virtio)

;; --- evdev event semantics (Linux input.h) ----------------------------------

(define EV-SYN 0)        ; frame boundary -- flush accumulated state
(define EV-KEY 1)        ; key/button: value 1=press 0=release 2=autorepeat
(define EV-REL 2)        ; relative axis: code 0=REL_X 1=REL_Y, value=signed delta
(define EV-ABS 3)        ; absolute axis: code 0=ABS_X 1=ABS_Y, value=position

(define REL-X 0)
(define REL-Y 1)
(define ABS-X 0)
(define ABS-Y 1)
(define BTN-LEFT #x110)  ; the primary mouse/tablet button keycode

;; --- pure decode: one virtio_input_event out of an 8-byte slot ---------------
;; Layout: type u16@0, code u16@2, value u32@4 (all LE). `value` is a SIGNED
;; 32-bit quantity (EV_REL deltas can be negative), so sign-extend the u32 into
;; a Lisp fixnum. Returns (list type code value).

(define (u32->s32 u)
  (if (>= u #x80000000) (- u #x100000000) u))

(define (parse-input-event buf off)
  (list (bytes-u16-ref buf off)
        (bytes-u16-ref buf (+ off 2))
        (u32->s32 (bytes-u32-ref buf (+ off 4)))))

;; --- pure decode: the event reducer ------------------------------------------
;; A "pstate" is the 5-tuple (x y down? ptr? emits):
;;   x/y    -- the current pointer position (absolute for a tablet; running for a
;;             mouse, seeded from the previous frame's position; carried across
;;             frames).
;;   down?  -- BTN_LEFT state (#t while held; carried across frames).
;;   ptr?   -- #t if a pointer field (motion or button) changed THIS frame, so the
;;             EV_SYN flush knows whether to emit a (pointer ...). Reset per frame.
;;   emits  -- payloads accumulated for the current frame, in REVERSE order;
;;             produced (forward order) on EV_SYN, then cleared.
;;
;; A non-pointer EV_KEY (a keyboard keycode, not BTN_LEFT) pushes (key code value)
;; into `emits` immediately -- keys have no frame accumulation. BTN_LEFT and
;; motion only set ptr?/x/y/down; the (pointer ...) payload is realised on EV_SYN.

(define (mk x y down ptr? emits) (list x y down ptr? emits))
(define (pstate0 x y) (mk x y #f #f '()))

;; The flushed payloads of a pstate, valid right after an EV_SYN reduce: the
;; (key ...)/(pointer ...) list to forward to coreinput, in FORWARD order.
(define (ps-out s) (nth s 4))

;; Fold one (type code value) event into the pstate, returning the new pstate.
(define (reduce-event s ev)
  (let ((type (nth ev 0)) (code (nth ev 1)) (value (nth ev 2))
        (x (nth s 0)) (y (nth s 1)) (down (nth s 2))
        (ptr? (nth s 3)) (emits (nth s 4)))
    (cond
      ;; absolute pointer (tablet): set x or y, mark pointer dirty.
      ((= type EV-ABS)
       (cond ((= code ABS-X) (mk value y down #t emits))
             ((= code ABS-Y) (mk x value down #t emits))
             (else s)))
      ;; relative pointer (mouse): accumulate delta into x/y, mark dirty.
      ((= type EV-REL)
       (cond ((= code REL-X) (mk (+ x value) y down #t emits))
             ((= code REL-Y) (mk x (+ y value) down #t emits))
             (else s)))
      ;; key / button.
      ((= type EV-KEY)
       (if (= code BTN-LEFT)
           ;; BTN_LEFT: update held state (value 1=press 0=release; ignore
           ;; autorepeat 2 for a button) and mark the frame a pointer frame so
           ;; EV_SYN emits a (pointer ...) reflecting the new button state.
           (cond ((= value 1) (mk x y #t #t emits))
                 ((= value 0) (mk x y #f #t emits))
                 (else (mk x y down #t emits)))
           ;; a real key: emit (key code value) immediately (no frame batching).
           (mk x y down ptr? (cons (list 'key code value) emits))))
      ;; EV_SYN: flush. If any pointer field changed this frame, append a single
      ;; (pointer x y down?) AFTER the keys (keys were pushed in reverse, so the
      ;; final per-frame order is keys-in-order then the pointer).
      ((= type EV-SYN)
       (let ((out (reverse (if ptr?
                               (cons (list 'pointer x y down) emits)
                               emits))))
         ;; emits for this *result* carries the flushed payloads (forward order);
         ;; reset ptr?, clear nothing about x/y/down (they persist across frames).
         (mk x y down #f out)))
      (else s))))

;; reduce-events: fold a list of (type code value) events through reduce-event,
;; returning (list final-pstate all-emitted-payloads). All payloads emitted at
;; every EV_SYN crossed are concatenated in order -- this is the host-test entry
;; point: feed a mock event sequence, assert the payloads.
(define (reduce-events x0 y0 evs)
  (let loop ((s (pstate0 x0 y0)) (evs evs) (acc '()))
    (if (null? evs)
        (list s (reverse acc))
        (let* ((type (nth (car evs) 0))
               (s2 (reduce-event s (car evs))))
          (if (= type EV-SYN)
              ;; flush: ps-out s2 is this frame's payloads (forward order);
              ;; carry forward a fresh pstate that keeps x/y/down but empties out.
              (loop (mk (nth s2 0) (nth s2 1) (nth s2 2) #f '())
                    (cdr evs)
                    (append (reverse (ps-out s2)) acc))
              (loop s2 (cdr evs) acc))))))

;; --- device config (select-based devcfg) ------------------------------------
;; Layout: select u8@0, subsel u8@1, size u8@2, data@8. Writing (select,subsel)
;; latches a value the device fills in; reading size@2 + data@8 returns it.

(define VIRTIO-INPUT-CFG-ID-NAME  #x01)
(define VIRTIO-INPUT-CFG-EV-BITS  #x11)
(define VIRTIO-INPUT-CFG-ABS-INFO #x12)

;; Probe whether the device advertises a given EV_* type by selecting
;; CFG_EV_BITS with subsel=type and checking the reported size is non-zero (the
;; bitmap of supported codes for that type). Used to classify the device.
(define (cfg-has-ev? devcfg evtype)
  (bytes-u8-set! devcfg 0 VIRTIO-INPUT-CFG-EV-BITS)
  (bytes-u8-set! devcfg 1 evtype)
  (> (bytes-u8-ref devcfg 2) 0))

;; ABS axis (min,max) from CFG_ABS_INFO (each u32 at data@8 and data@12). Returns
;; (min . max), or #f if the axis is unsupported (size 0).
(define (cfg-abs-info devcfg axis)
  (bytes-u8-set! devcfg 0 VIRTIO-INPUT-CFG-ABS-INFO)
  (bytes-u8-set! devcfg 1 axis)
  (if (= (bytes-u8-ref devcfg 2) 0)
      #f
      (cons (bytes-u32-ref devcfg 8) (bytes-u32-ref devcfg 12))))

;; --- eventq plumbing ---------------------------------------------------------

(define NEV 32)          ; eventq slots we keep posted
(define EVSLOT 8)        ; one virtio_input_event is 8 bytes

;; Post all NEV (capped to the ring) device-writable 8-byte buffers on the
;; eventq, descriptor i pointing at slot i.
(define (ev-populate! q evbuf notify mult)
  (let ((base (bytes-phys evbuf)) (desc (q-desc q)) (avail (q-avail q))
        (n (if (< (q-size q) NEV) (q-size q) NEV)))
    (let loop ((i 0))
      (if (= i n)
          (begin (notify-queue! notify mult q) n)
          (begin
            (desc-set! desc i (+ base (* i EVSLOT)) EVSLOT VIRTQ-DESC-F-WRITE 0)
            (avail-push! avail (q-size q) i)
            (loop (+ i 1)))))))

;; Drain newly-used eventq descriptors, calling (handler off) for each completed
;; event (off = byte offset of its 8-byte record in evbuf), then recycle the
;; descriptor back onto the avail ring.
(define (ev-drain! q evbuf last notify mult handler)
  (let ((used (q-used q)) (avail (q-avail q)) (qsize (q-size q)))
    (let loop ((li (cell-ref last)))
      (if (= li (bytes-u16-ref used 2))
          (cell-set! last li)
          (let* ((slot (modulo li qsize))
                 (id   (bytes-u32-ref used (+ 4 (* 8 slot)))))
            (handler (* id EVSLOT))
            (avail-push! avail qsize id)
            (notify-queue! notify mult q)
            (loop (bitwise-and (+ li 1) #xFFFF)))))))

;; --- bring-up ---------------------------------------------------------------

(define VIRTIO-INPUT-VID #x1af4)
(define VIRTIO-INPUT-DID #x1052)

;; virtio-input-init takes the coreinput service handle and the device's ECAM
;; (init enumerates 1af4:1052 via pci-find-all and binds one per device). It
;; brings the device up, registers with coreinput, and spawns the MSI-driven
;; event pump as a restricted context (so it can run under the scheduler), then
;; returns immediately -- *-init does NOT run under the scheduler.
(define (virtio-input-init coreinput dev-ecam)
  (let ((ecam dev-ecam))
    (if (not ecam)
        (begin (display "[virtio-input] no device present") (newline) #f)
        ;; Only VERSION_1 wanted (lo-want=0).
        (let ((dev (virtio-bringup ecam 0
                     (arithmetic-shift 1 VIRTIO-F-VERSION-1-BIT))))
          (if (not dev)
              (begin (display "[virtio-input] device rejected FEATURES_OK") (newline) #f)
              (let ((common (nth dev 0)) (devcfg (nth dev 1))
                    (notify (nth dev 2)) (mult (nth dev 3)))
                ;; Classify by advertised event types (best-effort; the pump is
                ;; type-driven regardless, so a misread only affects the log).
                (let* ((has-abs (cfg-has-ev? devcfg EV-ABS))
                       (has-rel (cfg-has-ev? devcfg EV-REL))
                       (kind (cond (has-abs 'virtio-tablet)
                                   (has-rel 'virtio-mouse)
                                   (else 'virtio-keyboard)))
                       ;; For a tablet, seed x/y at the ABS range midpoint so the
                       ;; first frame before any motion is sensible; for others 0.
                       (ax (cfg-abs-info devcfg ABS-X))
                       (ay (cfg-abs-info devcfg ABS-Y))
                       (x0 (if ax (arithmetic-shift (+ (car ax) (cdr ax)) -1) 0))
                       (y0 (if ay (arithmetic-shift (+ (car ay) (cdr ay)) -1) 0)))
                  ;; Set up the eventq (0); statusq (1) is left idle. Program the
                  ;; queue MSI-X vectors first, then msix_config, then enable the
                  ;; MSI cap (pci-setup-msi) -- the virtio-mandated order.
                  (let ((evq (virtio-setup-queue common 0)))
                    (if (not evq)
                        (begin (display "[virtio-input] eventq setup failed") (newline) #f)
                        (begin
                          (bytes-u16-set! common VIRTIO-MSIX-CONFIG 0)
                          (let ((msi (pci-setup-msi ecam)))
                            (if (not msi)
                                (begin (display "[virtio-input] MSI-X setup failed") (newline) #f)
                                (begin
                                  (virtio-status-set! common VIRTIO-STATUS-DRIVER-OK)
                                  (display "[virtio-input] up: kind=") (display kind)
                                  (display " msi=") (display msi) (newline)
                                  (let ((evbuf (dma-alloc (* NEV EVSLOT)))
                                        (last  (make-cell 0))
                                        (pst   (make-cell (pstate0 x0 y0))))
                                    (ev-populate! evq evbuf notify mult)
                                    (send coreinput (list 'register kind))
                                    ;; The event pump: a restricted context (no
                                    ;; import authority). It drains the eventq,
                                    ;; folds each event through the pure reducer,
                                    ;; and forwards the per-frame payloads to
                                    ;; coreinput tagged (event <payload>). Then it
                                    ;; parks until the next MSI. `seen` is captured
                                    ;; before draining so an event landing during
                                    ;; the drain advances the count and re-drains
                                    ;; rather than parking on a buffered event.
                                    (spawn-restricted '() (lambda ()
                                      (let pump ((seen (msi-count msi)))
                                        (ev-drain! evq evbuf last notify mult
                                          (lambda (off)
                                            (let* ((e (parse-input-event evbuf off))
                                                   (s2 (reduce-event (cell-ref pst) e)))
                                              (if (= (nth e 0) EV-SYN)
                                                  (begin
                                                    ;; flush this frame's payloads
                                                    ;; (ps-out is in forward order)
                                                    (for-each
                                                      (lambda (p)
                                                        (send coreinput (list 'event p)))
                                                      (ps-out s2))
                                                    ;; carry x/y/down, empty out.
                                                    (cell-set! pst
                                                      (mk (nth s2 0) (nth s2 1)
                                                          (nth s2 2) #f '())))
                                                  (cell-set! pst s2)))))
                                        (if (> (msi-count msi) seen)
                                            (pump (msi-count msi))
                                            (begin (msi-wait msi seen)
                                                   (pump (msi-count msi)))))))
                                    (display "[virtio-input] registered with coreinput") (newline)
                                    'ok))))))))))))))) ; close define-module
