;; nvme: an NVMe (NVM Express) PCIe block-storage driver in Cardinal Lisp.
;;
;; NVMe is a queue-pair + doorbell controller: the host posts fixed-size commands
;; into a Submission Queue (SQ) ring in host memory and rings that queue's tail
;; doorbell; the controller drains the SQ, executes, and posts a 16-byte
;; Completion Queue (CQ) entry whose PHASE bit toggles each ring wrap. We use the
;; admin queue (qid 0) to IDENTIFY the controller + namespace and to create one
;; I/O queue pair (qid 1) for READ/WRITE. Completion is detected by POLLING the
;; CQ phase bit (NOT MSI) -- the same "poll is truth" discipline ahci uses for
;; PxCI, so a missed/coalesced interrupt can never wedge a request.
;;
;; This is a single SQ entry in flight at a time: corestorage serialises requests
;; through the driver context's mailbox, so a depth-2 admin/IO ring (one slot
;; used) is sufficient and keeps the bring-up small.
;;
;; Capability imports, exactly as ahci: sys-mmio (mmio-map / dma-alloc /
;; dma-alloc-32 / bytes-phys), sys-pci (pci-find-class / pci-assign-bars /
;; pci-setup-msi -- MSI is set up but only for completeness; we poll), and the
;; driver-util helpers. The PURE builders/parsers (no sys-* calls) are exported
;; for the hardware-free host self-test: nvme-cmd-build! lays out a 64-byte SQE,
;; cq-status / cq-phase parse a CQE, and id-ns-nsze / id-ns-bsize parse IDENTIFY
;; NAMESPACE.
;;
;; CAVEAT -- request size. Each request uses PRP1 (+ PRP2 for a 2nd page), so a
;; single transfer is capped at 2 pages = 8 sectors of 512B (or 1 sector of 4K).
;; corestorage only bounds (lba+count) against the disk size, so the per-request
;; cap lives in the driver context (matching ahci's DATA-SECTORS cap).
;;
;; CAVEAT -- 64-bit. NSZE (namespace block count) is a u64; fixnums are 63-bit so
;; multi-exabyte disks would truncate. Test/real images are far below that.
(define-module nvme
  (export nvme-init
          ;; pure, host-testable:
          nvme-cmd-build! cq-status cq-phase
          id-ns-nsze id-ns-bsize id-ns-lbads
          NVME-OP-READ NVME-OP-WRITE
          NVME-AOP-IDENTIFY NVME-AOP-CREATE-CQ NVME-AOP-CREATE-SQ)
  (import sys-mmio sys-pci driver-util)
  (define lg (make-logger 'nvme))

  ;; =========================================================================
  ;; Controller register layout (BAR0).
  ;; =========================================================================
  (define REG-CAP   #x00)   ; u64: MQES 15:0, DSTRD 35:32
  (define REG-VS    #x08)
  (define REG-CC    #x14)   ; EN bit0, IOSQES 19:16, IOCQES 23:20
  (define REG-CSTS  #x1C)   ; RDY bit0
  (define REG-AQA   #x24)   ; ASQS 11:0, ACQS 27:16
  (define REG-ASQ   #x28)   ; u64 phys
  (define REG-ACQ   #x30)   ; u64 phys
  (define DOORBELL-BASE #x1000)

  (define CC-EN     #x00000001)
  (define CSTS-RDY  #x00000001)

  ;; A 32-bit MMIO register is the mapped BAR buffer read/written little-endian.
  (define (rd32 bar off)    (bytes-u32-ref bar off))
  (define (wr32 bar off v)  (bytes-u32-set! bar off v))
  ;; Write a 64-bit register as two 32-bit halves (low then high) -- some NVMe
  ;; controllers require/are happiest with 32-bit accesses to the 64-bit regs.
  (define (wr64 bar off v)
    (wr32 bar off       (bitwise-and v #xFFFFFFFF))
    (wr32 bar (+ off 4) (bitwise-and (arithmetic-shift v -32) #xFFFFFFFF)))

  ;; Doorbell stride: 4 << DSTRD bytes between consecutive doorbells. DSTRD is
  ;; CAP bits 35:32, i.e. bits 3:0 of the high CAP dword.
  (define (cap-dstrd bar)
    (bitwise-and (rd32 bar (+ REG-CAP 4)) #xF))
  (define (cap-mqes bar)
    (bitwise-and (rd32 bar REG-CAP) #xFFFF))   ; max queue entries - 1

  ;; SQyTDBL @ 0x1000 + (2y)   * (4 << DSTRD)
  ;; CQyHDBL @ 0x1000 + (2y+1) * (4 << DSTRD)
  (define (sq-doorbell-off dstrd y)
    (+ DOORBELL-BASE (* (* 2 y) (arithmetic-shift 4 dstrd))))
  (define (cq-doorbell-off dstrd y)
    (+ DOORBELL-BASE (* (+ (* 2 y) 1) (arithmetic-shift 4 dstrd))))

  ;; =========================================================================
  ;; Command opcodes.
  ;; =========================================================================
  (define NVME-AOP-CREATE-SQ #x01)   ; admin
  (define NVME-AOP-CREATE-CQ #x05)   ; admin
  (define NVME-AOP-IDENTIFY  #x06)   ; admin
  (define NVME-OP-WRITE      #x01)   ; I/O
  (define NVME-OP-READ       #x02)   ; I/O

  ;; =========================================================================
  ;; PURE: build a 64-byte NVMe Submission Queue Entry.
  ;; =========================================================================
  ;; The SQE is 16 little-endian dwords (NVMe 1.x Figure: Common Command Format):
  ;;   dword0  : CDW0 -- OPC 7:0, FUSE 9:8, PSDT 15:14, CID 31:16
  ;;   dword1  : NSID
  ;;   dword2-3: reserved
  ;;   dword4-5: MPTR (metadata ptr) -- 0
  ;;   dword6-7: PRP1 (u64)
  ;;   dword8-9: PRP2 (u64)
  ;;   dword10..15: CDW10..15 (command specific)
  ;; We zero the whole 64 bytes, then set the fields. cid is the command id echoed
  ;; back in the completion. prp1/prp2 are physical addresses (prp2 may be 0).
  ;; cdw10..cdw15 are a list of up to 6 dword values (missing -> 0).
  (define (nvme-cmd-build! sqe opc cid nsid prp1 prp2 cdws)
    (let loop ((i 0)) (if (< i 64) (begin (bytes-u8-set! sqe i 0) (loop (+ i 1))) 'z))
    ;; CDW0: opcode in 7:0, CID in 31:16.
    (bytes-u32-set! sqe 0 (bitwise-or (bitwise-and opc #xFF)
                                      (arithmetic-shift (bitwise-and cid #xFFFF) 16)))
    (bytes-u32-set! sqe 4 (bitwise-and nsid #xFFFFFFFF))      ; NSID
    (bytes-u32-set! sqe 24 (bitwise-and prp1 #xFFFFFFFF))     ; PRP1 lo (dword6)
    (bytes-u32-set! sqe 28 (bitwise-and (arithmetic-shift prp1 -32) #xFFFFFFFF))
    (bytes-u32-set! sqe 32 (bitwise-and prp2 #xFFFFFFFF))     ; PRP2 lo (dword8)
    (bytes-u32-set! sqe 36 (bitwise-and (arithmetic-shift prp2 -32) #xFFFFFFFF))
    ;; CDW10..15 start at byte 40.
    (let loop ((i 0) (cs cdws))
      (if (or (>= i 6) (null? cs))
          sqe
          (begin (bytes-u32-set! sqe (+ 40 (* i 4)) (bitwise-and (car cs) #xFFFFFFFF))
                 (loop (+ i 1) (cdr cs))))))

  ;; =========================================================================
  ;; PURE: parse a 16-byte Completion Queue Entry.
  ;; =========================================================================
  ;; CQE layout (4 dwords):
  ;;   dword0  : command-specific
  ;;   dword1  : reserved
  ;;   dword2  : SQHD 15:0, SQID 31:16
  ;;   dword3  : CID 15:0, Phase bit16, Status 31:17 (SC 24:17, SCT 27:25, ...)
  ;; `off` is the byte offset of the CQE in the CQ buffer.
  ;; cq-phase: the P (phase) bit -- 1 when this slot has been freshly written this
  ;; pass; the host tracks an expected phase that flips each ring wrap.
  (define (cq-phase cqbuf off)
    (bitwise-and (arithmetic-shift (bytes-u32-ref cqbuf (+ off 12)) -16) 1))
  ;; cq-status: the Status Field (bits 31:17 of dword3). 0 = success. We return the
  ;; full 15-bit status (SC+SCT+more) so any non-zero is an error.
  (define (cq-status cqbuf off)
    (arithmetic-shift (bytes-u32-ref cqbuf (+ off 12)) -17))
  (define (cq-cid cqbuf off)
    (bitwise-and (bytes-u32-ref cqbuf (+ off 12)) #xFFFF))

  ;; =========================================================================
  ;; PURE: parse IDENTIFY NAMESPACE (4096-byte structure).
  ;; =========================================================================
  ;;   NSZE  : u64 @ 0   -- namespace size in logical blocks (block count)
  ;;   FLBAS : u8  @ 26  -- bits 3:0 select an entry in the LBA Format list
  ;;   LBAF list @ 128   -- each entry is 4 bytes; LBADS = bits 23:16 -> the LBA
  ;;                        data size is 1 << LBADS (9 = 512, 12 = 4096).
  ;; id-ns-nsze composes the low 48 bits of NSZE (fixnums are 63-bit; that's far
  ;; beyond any test/real image).
  (define (id-ns-nsze idns)
    (bitwise-or (bytes-u32-ref idns 0)
                (arithmetic-shift (bitwise-and (bytes-u32-ref idns 4) #xFFFF) 32)))
  ;; The selected LBA format's LBADS exponent (bits 23:16 of the chosen LBAF dword).
  (define (id-ns-lbads idns)
    (let* ((flbas (bitwise-and (bytes-u8-ref idns 26) #xF))
           (lbaf  (bytes-u32-ref idns (+ 128 (* flbas 4)))))
      (bitwise-and (arithmetic-shift lbaf -16) #xFF)))
  ;; Block size in bytes = 1 << LBADS. Guard a zero/garbage LBADS -> 512.
  (define (id-ns-bsize idns)
    (let ((lbads (id-ns-lbads idns)))
      (if (or (< lbads 9) (> lbads 16)) 512 (arithmetic-shift 1 lbads))))

  ;; =========================================================================
  ;; The controller context.
  ;; =========================================================================
  ;; A queue is (buf phys entries entry-size). The controller state is a list:
  ;;   (bar dstrd asq acq iosq iocq aq-depth io-depth cid-cell admin-phase-cell
  ;;    io-phase-cell)
  ;; where *-cell are mutable cells (from a spawned context we can't set! a module
  ;; global, so per-queue head/phase live in driver-util make-cell byte cells).
  (define (q-buf q)     (nth q 0))
  (define (q-phys q)    (nth q 1))
  (define (q-entries q) (nth q 2))
  (define (q-esize q)   (nth q 3))
  (define (mk-q buf entries esize)
    (list buf (bytes-phys buf) entries esize))

  ;; Allocate a zeroed queue buffer of entries*esize bytes (page-aligned via
  ;; dma-alloc-32 -- NVMe queues must be physically contiguous + page-based; the
  ;; admin queue base must be 4K-aligned, which dma-alloc-32 satisfies).
  (define (alloc-q entries esize)
    (let* ((n (* entries esize))
           (buf (dma-alloc-32 n)))
      ;; Zero it: the CQ phase-bit protocol REQUIRES every completion slot start
      ;; at phase 0, else the first poll mistakes stale DMA garbage (phase 1 by
      ;; chance) for a real completion and reads a bogus status. dma-alloc is not
      ;; guaranteed to hand back zeroed memory.
      (let loop ((i 0))
        (if (< i n) (begin (bytes-u8-set! buf i 0) (loop (+ i 1))) buf))))

  ;; ctrl accessors (the threaded controller record).
  (define (c-bar c)        (nth c 0))
  (define (c-dstrd c)      (nth c 1))
  (define (c-asq c)        (nth c 2))
  (define (c-acq c)        (nth c 3))
  (define (c-iosq c)       (nth c 4))
  (define (c-iocq c)       (nth c 5))
  (define (c-aq-depth c)   (nth c 6))
  (define (c-io-depth c)   (nth c 7))
  (define (c-cid c)        (nth c 8))   ; cell: rolling command id
  (define (c-aphase c)     (nth c 9))   ; cell: admin CQ expected phase
  (define (c-iophase c)    (nth c 10))  ; cell: io CQ expected phase
  (define (c-atail c)      (nth c 11))  ; cell: admin SQ tail
  (define (c-itail c)      (nth c 12))  ; cell: io SQ tail
  (define (c-ahead c)      (nth c 13))  ; cell: admin CQ head
  (define (c-ihead c)      (nth c 14))  ; cell: io CQ head

  (define (next-cid c)
    (let ((v (cell-ref (c-cid c))))
      (cell-set! (c-cid c) (bitwise-and (+ v 1) #xFFFF))
      v))

  ;; --- submit one command on a queue pair and poll its completion -----------
  ;; nvme-cmd-build! writes a SQE from offset 0, so we build into a scratch and
  ;; copy it into the queue buffer at the slot offset (bytes-copy-into! from
  ;; driver-util; the substrate has no aliasing sub-buffer view).
  (define (build-into-slot! qbuf slot esize opc cid nsid prp1 prp2 cdws)
    (let ((scratch (make-bytes esize)))
      (nvme-cmd-build! scratch opc cid nsid prp1 prp2 cdws)
      (bytes-copy-into! qbuf (* slot esize) scratch esize)))

  ;; Submit one command on a queue pair and poll its completion. sq/cq are queue
  ;; records, depth is the ring depth, tail-cell/head-cell/phase-cell track the
  ;; ring positions for THIS pair, sq-dbl/cq-dbl are the doorbell register offsets.
  ;; Returns the 15-bit status (0 = ok) or -1 on a completion timeout. Single
  ;; command in flight: build at the current tail, ring the SQ doorbell, then poll
  ;; the CQ slot at the current head for the expected phase, advance + ring CQ.
  (define (submit! c sq cq depth tail-cell head-cell phase-cell sq-dbl cq-dbl
                   opc nsid prp1 prp2 cdws)
    (let* ((bar  (c-bar c))
           (cid  (next-cid c))
           (tail (cell-ref tail-cell))
           (head (cell-ref head-cell))
           (phase (cell-ref phase-cell)))
      (build-into-slot! (q-buf sq) tail (q-esize sq) opc cid nsid prp1 prp2 cdws)
      (let ((ntail (modulo (+ tail 1) depth)))
        (cell-set! tail-cell ntail)
        (wr32 bar sq-dbl ntail)
        (let* ((cq-off (* head (q-esize cq)))
               (done
                (let ((deadline (+ (uptime-ns) 5000000000)))
                  (let loop ()
                    (cond ((= (cq-phase (q-buf cq) cq-off) phase) #t)
                          ((> (uptime-ns) deadline) #f)
                          (else (sleep 100000) (loop)))))))
          (if (not done)
              -1
              (let ((st (cq-status (q-buf cq) cq-off))
                    (nhead (modulo (+ head 1) depth)))
                (cell-set! head-cell nhead)
                (if (= nhead 0) (cell-set! phase-cell (bitwise-xor phase 1)))
                (wr32 bar cq-dbl nhead)
                st))))))

  ;; --- admin / IO submit convenience ----------------------------------------
  (define (admin! c opc nsid prp1 prp2 cdws)
    (submit! c (c-asq c) (c-acq c) (c-aq-depth c)
             (c-atail c) (c-ahead c) (c-aphase c)
             (sq-doorbell-off (c-dstrd c) 0)
             (cq-doorbell-off (c-dstrd c) 0)
             opc nsid prp1 prp2 cdws))
  (define (io! c opc nsid prp1 prp2 cdws)
    (submit! c (c-iosq c) (c-iocq c) (c-io-depth c)
             (c-itail c) (c-ihead c) (c-iophase c)
             (sq-doorbell-off (c-dstrd c) 1)
             (cq-doorbell-off (c-dstrd c) 1)
             opc nsid prp1 prp2 cdws))

  ;; --- bring-up: disable, set admin queues, enable, identify, create IO QP ----
  (define (ctrl-disable! bar)
    (wr32 bar REG-CC (bitwise-and (rd32 bar REG-CC) (bitwise-not CC-EN)))
    (wait-until (lambda () (= 0 (bitwise-and (rd32 bar REG-CSTS) CSTS-RDY)))
                2000000000))

  (define (ctrl-enable! bar)
    ;; CC: IOSQES=6 (bits 19:16 -> 64B SQE), IOCQES=4 (bits 23:20 -> 16B CQE),
    ;; MPS=0 (4K pages, bits 10:7), CSS=0 (NVM command set, bits 6:4), EN=1.
    ;; (Spec layout: IOSQES is 19:16, IOCQES is 23:20 -- NOT the reverse.)
    (let ((cc (bitwise-or (arithmetic-shift 6 16)
                          (arithmetic-shift 4 20)
                          CC-EN)))
      (wr32 bar REG-CC cc))
    (wait-until (lambda () (not (= 0 (bitwise-and (rd32 bar REG-CSTS) CSTS-RDY))))
                2000000000))

  ;; AQA: ASQS in 11:0, ACQS in 27:16 -- both are (depth - 1).
  (define (set-admin-queues! bar asq acq depth)
    (wr32 bar REG-AQA (bitwise-or (bitwise-and (- depth 1) #xFFF)
                                  (arithmetic-shift (bitwise-and (- depth 1) #xFFF) 16)))
    (wr64 bar REG-ASQ (q-phys asq))
    (wr64 bar REG-ACQ (q-phys acq)))

  ;; Create the I/O completion queue (qid 1), then the I/O submission queue (qid 1)
  ;; pointed at it. CREATE CQ: prp1 = cq phys; cdw10 = (qsize-1)<<16 | qid;
  ;; cdw11 = PC bit0 (physically contiguous) | IEN bit1 (we leave IEN off -- poll).
  ;; CREATE SQ: prp1 = sq phys; cdw10 = (qsize-1)<<16 | qid;
  ;; cdw11 = PC bit0 | (cqid<<16).
  (define (create-io-queues! c qid depth iosq iocq)
    (let ((cq-st
           (admin! c NVME-AOP-CREATE-CQ 0 (q-phys iocq) 0
                   (list (bitwise-or (arithmetic-shift (- depth 1) 16) qid)
                         #x1))))            ; PC=1, IEN=0 (poll)
      (if (not (= cq-st 0))
          cq-st
          (admin! c NVME-AOP-CREATE-SQ 0 (q-phys iosq) 0
                  (list (bitwise-or (arithmetic-shift (- depth 1) 16) qid)
                        (bitwise-or #x1 (arithmetic-shift qid 16)))))))  ; PC=1, CQID=qid

  ;; =========================================================================
  ;; Driver request context (corestorage block-device protocol).
  ;; =========================================================================
  ;; Caps a request at MAX-SECTORS (one or two PRP pages). data-buf/data-phys is a
  ;; single shared DMA region (one request at a time -- corestorage serialises).
  ;; bsize is the namespace block size; PRP setup uses two entries: PRP1 = first
  ;; page, PRP2 = second page if the transfer spans into it.
  (define PAGE 4096)

  ;; Build (prp1 prp2) for a transfer of `len` bytes starting at data-phys. PRP1 is
  ;; the first page; if len > one page (offset 0), PRP2 is the next page. We require
  ;; data-phys page-aligned (dma-alloc-32 returns page-aligned buffers).
  (define (prp-pair data-phys len)
    (if (<= len PAGE)
        (list data-phys 0)
        (list data-phys (+ data-phys PAGE))))

  ;; Read/write `count` blocks at `lba` through the IO queue. SLBA in cdw10 (lo) +
  ;; cdw11 (hi); NLB (0-based) in cdw12 bits 15:0. Returns 0 / -1.
  (define (rw! c nsid opc lba count data-phys bsize)
    (let* ((len (* count bsize))
           (prp (prp-pair data-phys len))
           (st (io! c opc nsid (car prp) (cadr prp)
                    (list (bitwise-and lba #xFFFFFFFF)
                          (bitwise-and (arithmetic-shift lba -32) #xFFFFFFFF)
                          (bitwise-and (- count 1) #xFFFF)))))
      (if (= st 0) 0 -1)))

  (define (make-driver-ctx c nsid bsize max-sectors data-buf data-phys)
    (spawn-restricted '()
      (lambda ()
        (let loop ()
          (let ((m (recv)))
            (cond
              ((eq? (car m) 'read)
               (let ((lba (cadr m)) (count (caddr m)) (reply (cadddr m)))
                 (if (> count max-sectors)
                     (send reply (list 'complete -1 #f))
                     (let ((st (rw! c nsid NVME-OP-READ lba count data-phys bsize)))
                       (send reply
                             (list 'complete st
                                   (if (= st 0)
                                       (copy-bytes data-buf 0 (* count bsize))
                                       #f)))))))
              ((eq? (car m) 'write)
               (let ((lba (cadr m)) (count (caddr m))
                     (data (cadddr m)) (reply (nth m 4)))
                 (if (> count max-sectors)
                     (send reply (list 'complete -1))
                     (begin
                       (bytes-copy-into! data-buf 0 data (* count bsize))
                       (send reply (list 'complete
                                         (rw! c nsid NVME-OP-WRITE lba count
                                              data-phys bsize)))))))))
          (loop)))))

  ;; =========================================================================
  ;; Discovery + entry point.
  ;; =========================================================================
  (define BAR0 0)
  (define AQ-DEPTH 4)    ; admin queue depth (entries) -- tiny; one in flight
  (define IO-DEPTH 4)    ; io queue depth

  ;; Build the controller record (threaded through admin!/io!). All ring-position
  ;; trackers are mutable cells so the spawned bring-up/driver contexts can advance
  ;; them without set!-ing a module global.
  (define (make-ctrl bar dstrd asq acq iosq iocq)
    (list bar dstrd asq acq iosq iocq AQ-DEPTH IO-DEPTH
          (make-cell 0)        ; cid
          (make-cell 1)        ; admin phase  (init below)
          (make-cell 1)        ; io phase
          (make-cell 0)        ; admin tail
          (make-cell 0)        ; io tail
          (make-cell 0)        ; admin head
          (make-cell 0)))      ; io head

  ;; The bring-up body, run in a spawned (yielding) context. Resets the controller,
  ;; programs the admin queues, enables it, IDENTIFYs the controller + namespace 1,
  ;; creates the I/O queue pair, registers the block device, and runs a read smoke.
  (define (nvme-bringup bar ecam storage)
    (let* ((dstrd (cap-dstrd bar))
           (asq (alloc-q AQ-DEPTH 64))
           (acq (alloc-q AQ-DEPTH 16))
           (iosq (alloc-q IO-DEPTH 64))
           (iocq (alloc-q IO-DEPTH 16))
           (c (make-ctrl bar dstrd
                         (mk-q asq AQ-DEPTH 64) (mk-q acq AQ-DEPTH 16)
                         (mk-q iosq IO-DEPTH 64) (mk-q iocq IO-DEPTH 16))))
      ;; The ring trackers are initialised by make-ctrl (cid/tails/heads = 0,
      ;; expected phase = 1); no second init needed here.
      (if (not (ctrl-disable! bar))
          (begin (lg "disable timeout") 'fail)
          (begin
            (set-admin-queues! bar (c-asq c) (c-acq c) AQ-DEPTH)
            (if (not (ctrl-enable! bar))
                (begin (lg "enable (RDY) timeout") 'fail)
                ;; IDENTIFY NAMESPACE (CNS=0, nsid=1) -> NSZE + LBADS.
                (let ((idbuf (dma-alloc-32 4096)))
                  (let ((idst (admin! c NVME-AOP-IDENTIFY 1 (bytes-phys idbuf) 0
                                      (list 0))))   ; cdw10 CNS=0 (namespace)
                    (if (not (= idst 0))
                        (begin (lg "IDENTIFY NS failed st=" idst) 'fail)
                        (let ((nsze  (id-ns-nsze idbuf))
                              (bsize (id-ns-bsize idbuf)))
                          (lg "ns1 blocks=" nsze " bsize=" bsize)
                          (if (= nsze 0)
                              (begin (lg "empty namespace") 'fail)
                              ;; Create the I/O queue pair.
                              (let ((qst (create-io-queues! c 1 IO-DEPTH
                                                            (c-iosq c) (c-iocq c))))
                                (if (not (= qst 0))
                                    (begin (lg "create IO queues failed st=" qst) 'fail)
                                    ;; Up. MSI for completeness (we still poll).
                                    (let* ((msi (pci-setup-msi ecam))
                                           ;; cap one request to 2 pages worth of blocks
                                           (max-sec (quotient (* 2 PAGE) bsize))
                                           (dbuf (dma-alloc-32 (* max-sec bsize)))
                                           (drv (make-driver-ctx c 1 bsize max-sec
                                                                 dbuf (bytes-phys dbuf))))
                                      (nvme-smoke c bsize max-sec dbuf (bytes-phys dbuf) nsze)
                                      (send storage (list 'register-blockdev 'nvme0
                                                          bsize nsze drv))
                                      (lg "nvme0 registered: " nsze " x " bsize " byte blocks")
                                      'up)))))))))))))

  ;; A read smoke against the live device (read block 0); log only on failure.
  (define (nvme-smoke c bsize max-sec dbuf dbuf-phys nsze)
    (if (not (= 0 (rw! c 1 NVME-OP-READ 0 1 dbuf-phys bsize)))
        (begin (lg "smoke read block 0 FAILED"))))

  ;; nvme-init: bind to one NVMe controller. `ecam` is its ECAM config (passed by
  ;; init.clp per pci-find-class-all hit); `storage` is the corestorage handle.
  ;; Maps BAR0, enables mem+bus-master, then spawns the (yielding) bring-up. Like
  ;; ahci this is FIRE-AND-FORGET: it returns immediately so boot system-init is
  ;; never blocked on a recv under no scheduler.
  (define (nvme-init storage ecam)
    (if (not ecam)
        (begin (lg "no device present") #f)
        (let ((cfg (mmio-map ecam 4096)))
          (pci-enable-mem-bus-master! cfg)
          (let ((bar0 (let ((b (bar-base cfg BAR0)))
                        (if (= b 0)
                            (begin (pci-assign-bars ecam) (bar-base cfg BAR0))
                            b))))
            (if (or (not bar0) (= bar0 0))
                (begin (lg "no BAR0") #f)
                (let ((bar (mmio-map bar0 #x2000)))   ; regs + doorbells
                  (spawn-restricted '()
                    (lambda () (nvme-bringup bar ecam storage)))
                  'nvme-spawned)))))))
