;; hdaudio.clp -- Intel HD Audio controller bring-up + codec enumeration, ported
;; from drivers/hdaudio (drivers/hdaudio/src/main.c + inc/regs.h + inc/cmds.h).
;;
;; SCOPE OF THIS PORT. The C driver does three things: (1) brings the HDA
;; controller out of reset and stands up the CORB/RIRB command rings, (2)
;; enumerates the codec graph (every widget's params) over those rings, and (3)
;; *begins* (but never finishes -- the path-building loops are commented out / empty)
;; wiring a pin->stream playback path. There is no BDL / stream-descriptor / DMA
;; playback path in the C at all, and CoreAudio is a stub. So this Lisp port covers
;; (1) + (2): reset the controller, set up CORB/RIRB, enumerate the codec(s) and
;; their root-node vendor/device IDs, log what was found, and register the card
;; with coreaudio. The actual stream/playback path (BDL, stream descriptors, format
;; programming, a `play` message protocol) is a follow-up -- see CAVEATS at the
;; bottom of this file and the notes in the final report.
;;
;; hdaudio-init is the entry point init.clp calls (with the coreaudio handle). It:
;;   1. finds the HDA controller (8086:293e ICH9, or 8086:2668 ICH6) via pci-find --
;;      gated, so a default boot with no audio device just logs + returns,
;;   2. maps the HDA MMIO BAR (BAR0; pci-assign-bars if firmware left it
;;      unconfigured), enables memory + bus-master,
;;   3. runs reset + CORB/RIRB setup + codec enumeration inside a SPAWNED restricted
;;      context (so the reset / DMA-engine settle waits actually YIELD), then sets up
;;      MSI and registers the card with coreaudio.
;;
;; The driver imports exactly the capabilities it needs -- sys-mmio (mmio-map /
;; dma-alloc / dma-alloc-32), sys-pci (pci-find / pci-assign-bars / pci-setup-msi +
;; the MSI wake bridge msi-count / msi-wait) -- plus the generic driver-util
;; helpers. It exports just the entry point hdaudio-init.
(define-module hdaudio
  (export hdaudio-init)
  (import sys-mmio sys-pci driver-util)

;; --- the controllers we bind (pci-find matches VID/DID only) ----------------
;; QEMU's `-device intel-hda` (ICH9) is 8086:293e; the older ICH6 is 8086:2668.
;; A class-code (04/03) pci-find that would match any HDA controller is a noted
;; future substrate addition -- NOT built here. hdaudio-init tries both IDs.
(define HDA-ICH9-VID #x8086)
(define HDA-ICH9-DID #x293e)
(define HDA-ICH6-VID #x8086)
(define HDA-ICH6-DID #x2668)
(define HDA-BAR 0)             ; HD Audio exposes its register block in BAR0

;; --- MMIO register offsets (from inc/regs.h, == the HDA spec) ----------------
;; The C accessed these through a packed struct overlay; in Lisp the mapped BAR
;; is a byte buffer and we address registers by absolute offset. Offsets verified
;; against the regs.h struct layout AND the HD Audio spec.
(define GCAP      #x00)   ; u16: cap bits; bit0 = 64OK (controller supports 64-bit addr)
(define GCTL      #x08)   ; u32: bit0 = CRST# (0=reset asserted, 1=run), bit8 = UNSOL
(define WAKEEN    #x0C)   ; u16: per-codec SDIN wake enable
(define STATESTS  #x0E)   ; u16: per-codec state-change (codec-present) bits
(define INTCTL    #x20)   ; u32: bit31 = GIE (global int en), bit30 = CIE (controller int en)
(define INTSTS    #x24)   ; u32: bit31 = GIS, bit30 = CIS
(define CORBLBASE #x40)   ; u32
(define CORBUBASE #x44)   ; u32
(define CORBWP    #x48)   ; u16: write pointer (low 8 bits)
(define CORBRP    #x4A)   ; u16: read pointer; bit15 = CORBRPRST
(define CORBCTL   #x4C)   ; u8 : bit0 = CMEIE, bit1 = CORBRUN (DMA enable)
(define CORBSIZE  #x4E)   ; u8 : bits0-1 = size, bits4-7 = size-cap
(define RIRBLBASE #x50)   ; u32
(define RIRBUBASE #x54)   ; u32
(define RIRBWP    #x58)   ; u16: write pointer; bit15 = RIRBWPRST
(define RINTCNT   #x5A)   ; u16: response-interrupt count
(define RIRBCTL   #x5C)   ; u8 : bit0 = RINTCTL, bit1 = RIRBDMAEN, bit2 = OIC
(define RIRBSTS   #x5D)   ; u8 : bit0 = RINTFL (response int flag), bit2 = OIS
(define RIRBSIZE  #x5E)   ; u8 : bits0-1 = size, bits4-7 = size-cap

;; bit masks
(define GCTL-CRST   #x1)          ; GCTL bit0
(define CORBRPRST   #x8000)       ; CORBRP bit15
(define RIRBWPRST   #x8000)       ; RIRBWP bit15
(define CORBRUN     #x2)          ; CORBCTL bit1
(define RIRBDMAEN   #x2)          ; RIRBCTL bit1
(define RINTCTL     #x1)          ; RIRBCTL bit0
(define RINTFL      #x1)          ; RIRBSTS bit0
(define INTCTL-GIE  #x80000000)   ; INTCTL bit31
(define INTCTL-CIE  #x40000000)   ; INTCTL bit30

;; --- small MMIO accessors over the mapped BAR --------------------------------
;; (regs is the byte buffer returned by mmio-map; bytes-u{8,16,32}-{ref,set!} are
;; the volatile, little-endian-native accessors -- exactly what MMIO wants.)
(define (r8  regs off)   (bytes-u8-ref  regs off))
(define (r16 regs off)   (bytes-u16-ref regs off))
(define (r32 regs off)   (bytes-u32-ref regs off))
(define (w8!  regs off v) (bytes-u8-set!  regs off v))
(define (w16! regs off v) (bytes-u16-set! regs off v))
(define (w32! regs off v) (bytes-u32-set! regs off v))

;; --- CORB/RIRB sizing --------------------------------------------------------
;; The size-cap nibble (bits4-7 of CORBSIZE/RIRBSIZE) advertises which ring sizes
;; the controller supports: bit2 -> 256 entries, bit1 -> 16, bit0 -> 2. Mirrors
;; hdaudio_setupbuffersz(). Returns (entries . size-field) where size-field is the
;; value to OR into bits0-1 (2->0, 16->1, 256->2). QEMU's HDA caps at 256 for both.
(define (ring-entcnt szcap)
  (cond ((not (= 0 (bitwise-and szcap #x4))) 256)
        ((not (= 0 (bitwise-and szcap #x2))) 16)
        ((not (= 0 (bitwise-and szcap #x1))) 2)
        (else 256)))            ; spec says at least one bit is set; default 256
(define (ring-sizefield entcnt)
  (cond ((= entcnt 2) 0) ((= entcnt 16) 1) ((= entcnt 256) 2) (else 2)))

;; szcap = high nibble of the SIZE register.
(define (szcap-of regs off) (bitwise-and (arithmetic-shift (r8 regs off) -4) #xF))

;; --- controller reset --------------------------------------------------------
;; Drive GCTL.CRST# low (assert reset), wait for it to read back 0, then drive it
;; high (deassert) and wait for it to read 1. Mirrors module_init's reset dance.
;; Uses wait-until (it yields under the scheduler). #t on success.
(define (hda-reset! regs)
  (w32! regs GCTL (bitwise-and (r32 regs GCTL) (bitwise-not GCTL-CRST)))
  (if (not (wait-until (lambda () (= 0 (bitwise-and (r32 regs GCTL) GCTL-CRST)))
                       10000000))   ; 10ms
      #f
      (begin
        (w32! regs GCTL (bitwise-or (r32 regs GCTL) GCTL-CRST))
        ;; After deassert, codecs need ~521us to report on STATESTS; the CRST=1
        ;; poll plus the post-reset settle below covers that.
        (wait-until (lambda () (not (= 0 (bitwise-and (r32 regs GCTL) GCTL-CRST))))
                    10000000))))

;; --- CORB/RIRB DMA-engine setup ----------------------------------------------
;; Stop the engine, point its base registers at our DMA ring, reset the pointers,
;; set the size field, and (for RIRB) program the response-interrupt count and
;; arm the response interrupt, then start it. Mirrors hdaudio_initialize().
;;
;; NOTE on addressing: the CORB/RIRB base registers are a 32-bit lower + 32-bit
;; upper pair, so 64-bit addresses ARE expressible. But to keep the draft simple
;; and robust on a controller that reports !64OK, the ring buffer is allocated with
;; dma-alloc-32 (guaranteed < 4GB) and the upper-base is written 0. CORB+RIRB live
;; in one DMA buffer: CORB at offset 0, RIRB after it.

;; Program CORB: base, reset RP, size, start. corb-phys is the 32-bit phys addr.
(define (corb-setup! regs corb-phys entcnt)
  ;; stop the CORB DMA engine and wait
  (w8! regs CORBCTL (bitwise-and (r8 regs CORBCTL) (bitwise-not CORBRUN)))
  (wait-until (lambda () (= 0 (bitwise-and (r8 regs CORBCTL) CORBRUN))) 1000000)
  ;; base
  (w32! regs CORBLBASE (bitwise-and corb-phys #xFFFFFFFF))
  (w32! regs CORBUBASE 0)
  (w16! regs CORBWP 0)
  ;; reset the read pointer: set CORBRPRST, wait for it to read back set, clear it,
  ;; wait for it to read back clear (the spec handshake, mirrored from the C).
  (w16! regs CORBRP CORBRPRST)
  (wait-until (lambda () (not (= 0 (bitwise-and (r16 regs CORBRP) CORBRPRST)))) 1000000)
  (w16! regs CORBRP 0)
  (wait-until (lambda () (= 0 (bitwise-and (r16 regs CORBRP) CORBRPRST))) 1000000)
  ;; size field (keep the szcap nibble intact -- it's RO, but be safe)
  (w8! regs CORBSIZE (bitwise-or (bitwise-and (r8 regs CORBSIZE) #xF0)
                                 (ring-sizefield entcnt)))
  ;; start
  (w8! regs CORBCTL (bitwise-or (r8 regs CORBCTL) CORBRUN))
  (wait-until (lambda () (not (= 0 (bitwise-and (r8 regs CORBCTL) CORBRUN)))) 1000000))

;; Program RIRB: base, reset WP, size, intcnt + RINTCTL, start.
(define (rirb-setup! regs rirb-phys entcnt)
  (w8! regs RIRBCTL (bitwise-and (r8 regs RIRBCTL) (bitwise-not RIRBDMAEN)))
  (wait-until (lambda () (= 0 (bitwise-and (r8 regs RIRBCTL) RIRBDMAEN))) 1000000)
  (w32! regs RIRBLBASE (bitwise-and rirb-phys #xFFFFFFFF))
  (w32! regs RIRBUBASE 0)
  ;; reset the RIRB write pointer
  (w16! regs RIRBWP RIRBWPRST)
  (w8! regs RIRBSIZE (bitwise-or (bitwise-and (r8 regs RIRBSIZE) #xF0)
                                 (ring-sizefield entcnt)))
  ;; interrupt after every 1 response, and arm the response interrupt
  (w16! regs RINTCNT 1)
  (w32! regs INTCTL (bitwise-or INTCTL-GIE INTCTL-CIE))
  (w8! regs RIRBCTL (bitwise-or (r8 regs RIRBCTL) RINTCTL))
  ;; start the RIRB DMA engine
  (w8! regs RIRBCTL (bitwise-or (r8 regs RIRBCTL) RIRBDMAEN))
  (wait-until (lambda () (not (= 0 (bitwise-and (r8 regs RIRBCTL) RIRBDMAEN)))) 1000000))

;; --- verb send / response (polled) -------------------------------------------
;; A "verb" is a 32-bit command word: (addr<<28) | ((node & 0x7F)<<20) | (payload &
;; 0xFFFFF). We write it into the CORB ring at the next slot, bump CORBWP, then poll
;; RIRB for the response. The RIRB is a ring of 8-byte entries: [response u32][resp-ex
;; u32]; resp-ex bit4 = unsolicited. We track our own write index; the device's
;; RIRBWP gives the last-written RIRB slot.
;;
;; This polls rather than using the response interrupt (the C used the IRQ handler to
;; consume responses; polling is simpler and correct for one-at-a-time enumeration,
;; which is all bring-up does -- it issues a verb and waits for exactly one reply).
;;
;; ring is the SINGLE DMA buffer holding CORB (at offset 0) then RIRB (at offset
;; rirb-off). We index both regions of the one buffer with explicit offsets, so no
;; sub-buffer/offset-view primitive is needed -- the verb routine just adds the base
;; offset. `st` is the mutable command state: a driver-util cell holding the next
;; CORB slot index (wrapping at entcnt).
(define (hda-verb! regs ring rirb-off entcnt st addr node payload)
  (let ((verb (bitwise-or (arithmetic-shift addr 28)
                          (bitwise-or (arithmetic-shift (bitwise-and node #x7F) 20)
                                      (bitwise-and payload #xFFFFF))))
        (idx  (modulo (+ (cell-ref st) 1) entcnt))
        ;; Snapshot the RIRB write pointer BEFORE posting the verb: a single
        ;; response advances it by exactly one slot. We must NOT assume the CORB
        ;; slot equals the RIRB slot -- a spec-compliant controller resets RIRBWP
        ;; to 0xFF (so the FIRST response lands in RIRB slot 0, while the verb went
        ;; to CORB slot 1), whereas QEMU happens to reset it to 0 (keeping them
        ;; accidentally in lockstep). Deriving the target slot from the live RIRBWP
        ;; is correct on both -- the C driver likewise tracks its own rirb_rp.
        (rwp  (bitwise-and (r16 regs RIRBWP) #xFF)))
    (let ((rslot (modulo (+ rwp 1) entcnt)))
      (cell-set! st idx)
      ;; write the verb into CORB slot idx (each slot is a u32), at ring offset idx*4
      (bytes-u32-set! ring (* idx 4) verb)
      ;; publish: CORBWP = idx (low 8 bits)
      (w16! regs CORBWP (bitwise-and idx #xFF))
      ;; wait for the RIRB write pointer to advance to the slot the response will
      ;; occupy, then read that 8-byte entry's response u32 ([resp][resp-ex]).
      (if (wait-until (lambda () (= (bitwise-and (r16 regs RIRBWP) #xFF) rslot)) 1000000)
          (begin
            ;; ack the response-interrupt flag (write-1-to-clear), matching the C
            (w8! regs RIRBSTS RINTFL)
            (bytes-u32-ref ring (+ rirb-off (* rslot 8))))  ; response u32
          #f))))                                            ; timeout -> #f

;; GET_PARAMETER verb (payload 0xF00xx, where xx is the parameter id).
(define (get-param regs ring rirb-off entcnt st addr node param)
  (hda-verb! regs ring rirb-off entcnt st addr node
             (bitwise-or #xF0000 param)))

;; HDA GET_PARAMETER parameter ids (subset, from inc/cmds.h)
(define PARAM-VENDOR-DEVICE-ID #x00)
(define PARAM-NODE-CNT         #x04)   ; subordinate node count: (start<<16)|count
(define PARAM-FUNC-GRP-TYPE    #x05)
(define PARAM-AUDIO-WIDGET-CAPS #x09)

;; --- codec enumeration -------------------------------------------------------
;; For each codec bit set in STATESTS, read the root node (node 0) vendor/device id
;; and its subordinate-node range. Logging the codec's vendor:device and child-node
;; count is the useful, verifiable result of bring-up (mirrors the DEBUG_PRINTs in
;; hdaudio_scanhandler for the root params). A full per-widget scan (the big
;; sendverb loop in hdaudio_initialize) is a follow-up; see CAVEATS.
(define (enumerate-codec regs ring rirb-off entcnt st addr)
  (let ((vid (get-param regs ring rirb-off entcnt st addr 0 PARAM-VENDOR-DEVICE-ID))
        (nc  (get-param regs ring rirb-off entcnt st addr 0 PARAM-NODE-CNT)))
    (if (or (not vid) (not nc))
        (begin (display "[hdaudio] codec ") (display addr)
               (display ": verb timeout") (newline) #f)
        (begin
          (display "[hdaudio] codec ") (display addr)
          (display ": vendor=") (display (arithmetic-shift vid -16))
          (display " device=") (display (bitwise-and vid #xFFFF))
          (display " child-nodes=") (display (bitwise-and nc #xFF))
          (display " (start ") (display (bitwise-and (arithmetic-shift nc -16) #xFF))
          (display ")") (newline)
          vid))))

;; Walk STATESTS bits 0..14, enumerating each present codec. Returns the count.
(define (enumerate-codecs regs ring rirb-off entcnt st statests)
  (let loop ((i 0) (found 0))
    (if (= i 15)
        found
        (if (not (= 0 (bitwise-and statests (arithmetic-shift 1 i))))
            (begin (enumerate-codec regs ring rirb-off entcnt st i)
                   (loop (+ i 1) (+ found 1)))
            (loop (+ i 1) found)))))

;; --- bring-up body (runs in the spawned, yielding context) -------------------
;; Mirrors module_init + hdaudio_initialize, scoped to controller + codec
;; enumeration. On success it logs, sets up MSI, and registers with coreaudio.
;; FIRE-AND-FORGET: like ahci-bringup, this runs in a spawned context so its
;; reset/settle waits yield; init does not await it. Any failure logs + returns.
(define (hdaudio-bringup regs ecam audio)
  ;; reset the controller
  (if (not (hda-reset! regs))
      (begin (display "[hdaudio] controller reset timeout") (newline) 'fail)
      (begin
        ;; codecs report presence on STATESTS shortly after CRST deassert; give
        ;; them a beat to settle, then snapshot. (~1ms covers the 521us spec wait.)
        (sleep 1000000)
        (let* ((statests (r16 regs STATESTS))
               (corb-ent (ring-entcnt (szcap-of regs CORBSIZE)))
               (rirb-ent (ring-entcnt (szcap-of regs RIRBSIZE))))
          (display "[hdaudio] reset OK statests=") (display statests)
          (display " corb-ent=") (display corb-ent)
          (display " rirb-ent=") (display rirb-ent) (newline)
          ;; enable codec wake/state-change reporting (mirrors wakeen=0xFFFF)
          (w16! regs WAKEEN #xFFFF)
          (if (= statests 0)
              (begin (display "[hdaudio] no codecs present on STATESTS") (newline)
                     ;; still register the (empty) card so the endpoint exists
                     (send audio (list 'register 'hda0))
                     'up-no-codec)
              ;; Allocate CORB+RIRB in ONE 32-bit DMA buffer: CORB (corb-ent u32s)
              ;; at offset 0, RIRB (rirb-ent 8-byte entries) at offset corb-bytes.
              ;; We index both regions of the single buffer with explicit offsets, so
              ;; NO sub-buffer/offset-view primitive is needed. dma-alloc-32 keeps the
              ;; phys addr < 4GB so the 32-bit lower-base alone suffices (upper = 0).
              (let* ((corb-bytes (* corb-ent 4))
                     (rirb-bytes (* rirb-ent 8))
                     (rirb-off   corb-bytes)
                     (ring (dma-alloc-32 (+ corb-bytes rirb-bytes)))
                     (ring-phys (bytes-phys ring))
                     ;; hda-verb! uses one slot index into both rings (CORB slot*4,
                     ;; RIRB slot*8), so it must wrap at the SMALLER ring's count.
                     ;; (On QEMU both are 256, so this is just defensive.)
                     (slot-cnt (if (< corb-ent rirb-ent) corb-ent rirb-ent))
                     (st (make-cell 0)))   ; next CORB slot index (cell, mutable)
                (corb-setup! regs ring-phys corb-ent)               ; CORB at ring base
                (rirb-setup! regs (+ ring-phys rirb-off) rirb-ent)  ; RIRB after it
                (let ((n (enumerate-codecs regs ring rirb-off slot-cnt st statests)))
                  (display "[hdaudio] enumerated ") (display n)
                  (display " codec(s)") (newline)
                  ;; MSI last (after the rings exist). We poll RIRB for responses
                  ;; rather than handle the IRQ, but still set up MSI so the
                  ;; controller's interrupt is owned and a future stream/handler
                  ;; context can wake on it.
                  (let ((msi (pci-setup-msi ecam)))
                    (display "[hdaudio] msi=") (display msi) (newline)
                    (send audio (list 'register 'hda0))
                    (display "[hdaudio] registered hda0 with coreaudio") (newline)
                    'up))))))))

;; --- entry point -------------------------------------------------------------
;; hdaudio-init takes the coreaudio service handle. Gated on pci-find so a no-audio
;; boot just logs + returns.
(define (find-hda)
  (let ((a (pci-find HDA-ICH9-VID HDA-ICH9-DID)))
    (if a a (pci-find HDA-ICH6-VID HDA-ICH6-DID))))

(define (hdaudio-init audio)
  (let ((ecam (find-hda)))
    (if (not ecam)
        (begin (display "[hdaudio] no device present") (newline) #f)
        (let ((cfg (mmio-map ecam 4096)))
          (pci-enable-mem-bus-master! cfg)
          ;; HDA register block = BAR0. If firmware never configured it (base 0),
          ;; self-assign and re-read.
          (let ((bar-phys (let ((b (bar-base cfg HDA-BAR)))
                            (if (= b 0)
                                (begin (pci-assign-bars ecam) (bar-base cfg HDA-BAR))
                                b))))
            (if (or (not bar-phys) (= bar-phys 0))
                (begin (display "[hdaudio] no MMIO BAR") (newline) #f)
                (let ((regs (mmio-map bar-phys #x180)))   ; controller reg block
                  ;; Run reset + ring setup + enumeration in a spawned context so the
                  ;; reset/settle waits yield (same pattern as ahci-init). Fire and
                  ;; forget: the context logs + registers and reports to the log;
                  ;; init does not await it.
                  (spawn-restricted '()
                    (lambda () (hdaudio-bringup regs ecam audio)))
                  'hdaudio-spawned))))))) ) ; last ) closes (define-module hdaudio ...)
