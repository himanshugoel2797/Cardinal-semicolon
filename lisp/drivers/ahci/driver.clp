;; ahci/driver.clp -- discovery, bring-up context, and the block-device driver
;; context that answers corestorage (read/write lba count ...) requests.
;;
;; ahci-init is the entry point init.clp calls (with the corestorage handle). It:
;;   1. finds the QEMU ICH9 AHCI controller (8086:2922) via pci-find -- gated, so
;;      a default boot with no disk just logs + returns,
;;   2. maps the ABAR (BAR5; pci-assign-bars if firmware left it unconfigured),
;;      enables memory + bus-master,
;;   3. runs HBA/port bring-up + IDENTIFY inside a SPAWNED context (so the reset/
;;      spin-up yields work), then sets up MSI and the driver request context,
;;   4. registers the block device with corestorage.

;; QEMU's ICH9 AHCI controller. NOTE: pci-find matches on VID/DID only, so this
;; binds the ICH9 specifically; a class-code (01/06) pci-find that would match any
;; AHCI HBA is a noted future substrate addition -- NOT built here.
(define AHCI-VID #x8086)
(define AHCI-DID #x2922)
(define ABAR-BAR 5)

;; The block-device driver context. It owns the AHCI command machinery (the ctx
;; threaded through issue!) and a data DMA buffer, and answers corestorage:
;;   (read  lba count reply) -> (send reply (list 'complete status bytes))
;;   (write lba count data  reply) -> (send reply (list 'complete status))
;; status 0 = ok, -1 = error. The data buffer is sized for one request at a time
;; (corestorage serialises through this single context's mailbox).
(define (make-driver-ctx ctx data-buf data-phys s64a?)
  (spawn-restricted '()
    (lambda ()
      (let loop ()
        (let ((m (recv)))
          (cond
            ((eq? (car m) 'read)
             ;; Cap count at DATA-SECTORS: the data DMA buffer is that big and one
             ;; PRD entry bounds the transfer, so a larger request would overrun
             ;; the buffer. corestorage only bounds (lba+count) against bcount, not
             ;; count alone, so the cap lives here.
             (let* ((lba (cadr m)) (count (caddr m)) (reply (cadddr m)))
               (if (> count DATA-SECTORS)
                   (send reply (list 'complete -1 #f))
                   (let ((st (read-sectors ctx lba count data-phys)))
                     (send reply
                           (list 'complete st
                                 (if (= st 0) (copy-bytes data-buf 0 (* count 512)) #f)))))))
            ((eq? (car m) 'write)
             (let* ((lba (cadr m)) (count (caddr m))
                    (data (cadddr m)) (reply (nth m 4)))
               (if (> count DATA-SECTORS)
                   (send reply (list 'complete -1))
                   (begin
                     (bytes-copy-into! data-buf 0 data (* count 512))
                     (send reply (list 'complete (write-sectors ctx lba count data-phys))))))))
          (loop))))))

;; The data buffer is large enough for a multi-sector request; one PRD entry caps a
;; transfer at 4MB, so DATA-SECTORS keeps a single request inside one PRD.
(define DATA-SECTORS 8)   ; up to 8 sectors (4KB) per request -- plenty for blocks

;; ahci-init: discover, bring up, register. `storage` is the corestorage handle.
(define (ahci-init storage)
  (let ((ecam (pci-find AHCI-VID AHCI-DID)))
    (if (not ecam)
        (begin (display "[ahci] no device present") (newline) #f)
        (let ((cfg (mmio-map ecam 4096)))
          (pci-enable-mem-bus-master! cfg)
          ;; ABAR = BAR5. If firmware never configured it (base 0), self-assign and
          ;; RE-READ BAR5 -- pci-assign-bars returns the device's FIRST BAR (BAR0,
          ;; a legacy IDE I/O range for the ICH9), not the ABAR, so its return value
          ;; must be discarded and BAR5 read back (the rtl8169/rtl8139 pattern).
          (let ((abar-phys (let ((b (bar-base cfg ABAR-BAR)))
                             (if (= b 0)
                                 (begin (pci-assign-bars ecam) (bar-base cfg ABAR-BAR))
                                 b))))
            (if (or (not abar-phys) (= abar-phys 0))
                (begin (display "[ahci] no ABAR") (newline) #f)
                (let ((abar (mmio-map abar-phys #x1100)))   ; HBA regs + 32 ports
                  ;; Run discovery + bring-up + IDENTIFY in a spawned context so the
                  ;; reset / spin-up / completion waits actually YIELD. This is
                  ;; FIRE-AND-FORGET: the init context that calls ahci-init is the
                  ;; boot system-init, which is NOT itself running under the
                  ;; scheduler -- it can `spawn` but a blocking `recv` here errors
                  ;; "not running under a scheduler" and wedges system-init. So the
                  ;; bring-up context does its own logging + registration + smoke and
                  ;; reports to the log; init does not await it. This mirrors
                  ;; virtio-net-init / virtio-gpu-init, which likewise spawn their
                  ;; bring-up/pump contexts and return without blocking.
                  (spawn-restricted '()
                    (lambda () (ahci-bringup abar ecam storage)))
                  'ahci-spawned)))))))

;; Try each implemented port in turn; return (port regions) for the first whose
;; link comes up + device is ready, or #f if none do. Each candidate gets its own
;; freshly-allocated DMA regions (the winner's are the ones we keep). NOTE: there is
;; no dma-free primitive (foreign DMA buffers are never GC'd), so the regions of
;; rejected ports leak (~1.8KB each). Bring-up is one-shot at boot, so this is a
;; bounded, small loss; a dma-free would let us reclaim it.
(define (find-live-port abar ports s64a?)
  (if (null? ports)
      #f
      (let* ((port (car ports))
             (regions (port-alloc-regions s64a?)))
        (if (port-bringup! abar port regions s64a?)
            (list port regions)
            (find-live-port abar (cdr ports) s64a?)))))

;; The bring-up body (runs in a spawned, yielding context). On success it logs the
;; model + sector count, sets up MSI, arms the port/global interrupt LAST (after
;; the driver context exists), registers the block device, runs the P2/P3 smoke,
;; and returns 'up. Any failure logs + returns 'fail. The caller relays the result.
(define (ahci-bringup abar ecam storage)
  (let ((s64a? (hba-s64a? abar)))
    (hba-handoff! abar)
    (if (not (hba-reset! abar))
        (begin (display "[ahci] HBA reset timeout") (newline) 'fail)
        (let ((ports (hba-ports abar)))
          (if (null? ports)
              (begin (display "[ahci] no ports implemented") (newline) 'fail)
              ;; Find the first IMPLEMENTED port whose link comes up + device is
              ;; ready. A controller may carry its disk on any port (the QEMU q35
              ;; built-in AHCI exposes several empty ports), so scan rather than
              ;; assume port 0 -- an empty port's link wait just fails and we move on.
              (let ((live (find-live-port abar ports s64a?)))
                (if (not live)
                    (begin (display "[ahci] no port came up (no device/link)")
                           (newline) 'fail)
                    (let ((port    (car live))
                          (regions (cadr live)))
                    ;; Port is up. IDENTIFY with a poll-only ctx (MSI added after).
                    (let* ((idbuf  (if s64a? (dma-alloc 512) (dma-alloc-32 512)))
                           (poll-ctx (list abar port regions s64a? #f))
                           (idst (identify poll-ctx (bytes-phys idbuf))))
                      (if (not (= idst 0))
                          (begin (display "[ahci] IDENTIFY failed") (newline) 'fail)
                          (let ((sectors (id-sector-count idbuf))
                                (model   (id-model idbuf)))
                            (display "[ahci] up: model=") (display model)
                            (display " sectors=") (display sectors) (newline)
                            ;; --- P2/P3: MSI + driver context + registration ---
                            (let ((msi (pci-setup-msi ecam)))
                              (display "[ahci] msi=") (display msi) (newline)
                              (let* ((io-ctx (list abar port regions s64a? msi))
                                     (dbuf (if s64a? (dma-alloc (* DATA-SECTORS 512))
                                               (dma-alloc-32 (* DATA-SECTORS 512))))
                                     (driver-ctx (make-driver-ctx io-ctx dbuf
                                                                  (bytes-phys dbuf) s64a?)))
                                ;; IRQ-last: arm PxIE + GHC.IE only now the driver
                                ;; context exists to receive completions (and so an
                                ;; MSI can fire during the smoke below).
                                (px-wr abar port PxIE PxIE-MASK)
                                (wr32 abar HBA-GHC (bitwise-or (rd32 abar HBA-GHC) GHC-IE))
                                ;; --- P2/P3 live smoke (read sector 0; write+read
                                ;; back a scratch sector) directly via io-ctx. Run
                                ;; BEFORE registration so this context has EXCLUSIVE
                                ;; access to io-ctx + dbuf -- once registered, the
                                ;; driver-ctx may be handed a (read ...) by
                                ;; corestorage and issue a second slot-0 command into
                                ;; the same buffer, racing the smoke (it yields).
                                (ahci-smoke io-ctx msi sectors dbuf (bytes-phys dbuf))
                                (send storage (list 'register-blockdev 'ahci0 512
                                                    sectors driver-ctx))
                                (display "[ahci] registered ahci0 with corestorage")
                                (newline)
                                'up)))))))))))))

;; Live read/write smoke against a real device, run once at bring-up.
;;   P2: read LBA 0 -> log the first bytes + the 0x55AA MBR signature at offset 510.
;;   P3: write a known pattern at a high SCRATCH LBA (sectors-8, never the MBR),
;;       read it back, and assert equality -> "ahci writeback OK".
;; Snapshots msi-count around the first read to report whether MSI fired or the
;; PxCI poll carried completion (the second-slot validation the plan asks for).
(define (ahci-smoke ctx msi sectors dbuf dbuf-phys)
  ;; P2: read sector 0.
  (let ((before (if msi (msi-count msi) 0)))
    (if (not (= 0 (read-sectors ctx 0 1 dbuf-phys)))
        (begin (display "[ahci] P2 read sector 0 FAILED") (newline))
        (begin
          (display "[ahci] P2 sector0[0..3]=")
          (display (bytes-u8-ref dbuf 0)) (display " ")
          (display (bytes-u8-ref dbuf 1)) (display " ")
          (display (bytes-u8-ref dbuf 2)) (display " ")
          (display (bytes-u8-ref dbuf 3))
          (display " mbr-sig=")
          (display (bytes-u8-ref dbuf 510)) (display ",")
          (display (bytes-u8-ref dbuf 511))
          (display (if (and (= (bytes-u8-ref dbuf 510) #x55)
                            (= (bytes-u8-ref dbuf 511) #xAA))
                       " (0x55AA seen)" " (no 0x55AA)"))
          (newline)
          ;; The MSI fires and is serviced here: the device raises its interrupt
          ;; (PxIS/HBA-IS set, GHC.IE on) and the CPU runs the handler, advancing
          ;; msi-count. This works because the live scheduler runs interrupts-ON
          ;; (see lisp_core_loop in SysLisp) -- earlier the evaluator ran with
          ;; interrupts masked, so every device MSI piled up unserviced in the IRR
          ;; and completion fell back to the poll. issue! STILL treats the PxCI
          ;; poll as the authoritative completion signal and uses msi-wait only to
          ;; yield the core, so a missed/coalesced MSI can never wedge a request;
          ;; msi-count reports whether the MSI actually carried this one.
          (display (if (and msi (> (msi-count msi) before))
                       "[ahci] MSI fired (msi-count advanced)"
                       "[ahci] completion via PxCI-poll (no MSI delivered -- see header)"))
          (newline))))
  ;; P3: write a known pattern at a scratch high LBA, read it back, compare.
  (let ((scratch (- sectors 8)))
    ;; Stamp a recognisable pattern into dbuf (512 bytes).
    (let loop ((i 0)) (if (< i 512)
                          (begin (bytes-u8-set! dbuf i (bitwise-and (+ i #xA5) #xFF))
                                 (loop (+ i 1))) 'z))
    (if (not (= 0 (write-sectors ctx scratch 1 dbuf-phys)))
        (begin (display "[ahci] P3 write FAILED") (newline))
        (begin
          ;; Clobber dbuf, then read it back and verify every byte matches.
          (let loop ((i 0)) (if (< i 512) (begin (bytes-u8-set! dbuf i 0) (loop (+ i 1))) 'z))
          (if (not (= 0 (read-sectors ctx scratch 1 dbuf-phys)))
              (begin (display "[ahci] P3 readback FAILED") (newline))
              (let ((ok (let loop ((i 0))
                          (cond ((= i 512) #t)
                                ((= (bytes-u8-ref dbuf i) (bitwise-and (+ i #xA5) #xFF))
                                 (loop (+ i 1)))
                                (else #f)))))
                (display (if ok "[ahci] writeback OK" "[ahci] writeback MISMATCH"))
                (newline)))))))
