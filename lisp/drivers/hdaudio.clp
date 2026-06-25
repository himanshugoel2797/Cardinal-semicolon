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
;;   1. finds the HDA controller by PCI class (0x04/0x03, any HD Audio controller)
;;      via pci-find-class -- gated, so a default boot with no audio device just
;;      logs + returns,
;;   2. maps the HDA MMIO BAR (BAR0; pci-assign-bars if firmware left it
;;      unconfigured), enables memory + bus-master,
;;   3. runs reset + CORB/RIRB setup + codec enumeration inside a SPAWNED restricted
;;      context (so the reset / DMA-engine settle waits actually YIELD), then sets up
;;      MSI and registers the card with coreaudio.
;;
;; The driver imports exactly the capabilities it needs -- sys-mmio (mmio-map /
;; dma-alloc / dma-alloc-32), sys-pci (pci-find-class / pci-assign-bars /
;; pci-setup-msi + the MSI wake bridge msi-count / msi-wait) -- plus the generic
;; driver-util
;; helpers. It exports just the entry point hdaudio-init.
(define-module hdaudio
  (export hdaudio-init)
  (import sys-mmio sys-pci driver-util)

;; --- the controllers we bind (matched by PCI class, not VID/DID) -------------
;; Every HD Audio controller advertises base class 0x04 (multimedia) subclass 0x03
;; (HD Audio), so pci-find-class binds ANY of them -- QEMU's intel-hda (8086:2668)
;; and ich9-intel-hda (8086:293e), plus real Intel/NVIDIA/AMD/VIA HDA controllers
;; -- without an ever-growing VID/DID table.
(define HDA-CLASS    #x04)     ; PCI base class: multimedia controller
(define HDA-SUBCLASS #x03)     ; PCI subclass: HD Audio
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
(define PARAM-FUNC-GRP-TYPE    #x05)   ; low 7 bits: 1 = audio function group
(define PARAM-AUDIO-WIDGET-CAPS #x09)  ; bits20-23 = widget type
(define PARAM-PIN-CAPS         #x0C)   ; bit4 = output-capable, bit5 = input-capable
(define PARAM-CONN-LIST-LEN    #x0E)   ; bits0-6 = length, bit7 = long-form
(define PARAM-OUTPUT-AMP-CAPS  #x12)   ; bits8-14 = num-steps (max gain)

;; widget types (audio-widget-caps bits20-23)
(define WIDGET-AUDIO-OUTPUT 0)   ; a DAC (converter: stream -> analog)
(define WIDGET-PIN-COMPLEX  4)   ; a jack/pin
(define PIN-CAP-OUTPUT      #x10) ; pin-caps bit4

;; --- codec handle + verb builders -------------------------------------------
;; A "codec handle" bundles everything hda-verb! needs except the target node and
;; the verb payload, so the path-discovery / configuration code can talk to a
;; codec without threading six arguments through every call.
(define (make-cdc regs ring rirb-off slot-cnt st addr)
  (list regs ring rirb-off slot-cnt st addr))
(define (cdc-verb cdc node payload)
  (hda-verb! (nth cdc 0) (nth cdc 1) (nth cdc 2) (nth cdc 3) (nth cdc 4) (nth cdc 5)
             node payload))
(define (cdc-param cdc node param) (cdc-verb cdc node (bitwise-or #xF0000 param)))

;; The two verb encodings (HDA spec 7.3.1): a 12-bit verb id + 8-bit data, or a
;; 4-bit verb id + 16-bit data, packed into the 20-bit verb payload.
(define (v12 verb data) (bitwise-or (arithmetic-shift verb 8)  (bitwise-and data #xFF)))
(define (v4  verb data) (bitwise-or (arithmetic-shift verb 16) (bitwise-and data #xFFFF)))

;; verb ids we issue (HDA spec 7.3.3)
(define VERB-SET-CONVERTER-FORMAT   #x2)    ; 4-bit  + 16-bit format
(define VERB-SET-AMP-GAIN-MUTE      #x3)    ; 4-bit  + 16-bit gain/mute
(define VERB-SET-CONN-SELECT      #x701)    ; 12-bit + conn-list index
(define VERB-SET-POWER-STATE      #x705)    ; 12-bit + power state (0 = D0)
(define VERB-SET-STREAM-CHANNEL   #x706)    ; 12-bit + (stream<<4)|channel
(define VERB-SET-PIN-CONTROL      #x707)    ; 12-bit + pin-control bits
(define VERB-SET-EAPD-BTL         #x70C)    ; 12-bit + EAPD/BTL bits
(define VERB-GET-CONN-LIST-ENTRY  #xF02)    ; 12-bit + entry offset

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

;; --- output-path discovery ---------------------------------------------------
;; The C driver enumerated every widget but never used the graph (the path loops
;; are commented out). To actually play, we walk the codec graph far enough to
;; find ONE output path: an audio-output converter (DAC) plus an output-capable
;; pin in the same audio function group. That is all a single playback stream
;; needs; richer routing (mixers/selectors, multiple jacks) is a follow-up.

;; The lowest set STATESTS bit identifies the codec address to drive.
(define (first-codec-addr statests)
  (let loop ((i 0))
    (cond ((= i 15) 0)
          ((not (= 0 (bitwise-and statests (arithmetic-shift 1 i)))) i)
          (else (loop (+ i 1))))))

;; Within an audio function group, find the first DAC and the first output-capable
;; pin. Returns (list dac pin) or #f if either is absent.
(define (find-dac-pin cdc afg)
  (let ((nc (cdc-param cdc afg PARAM-NODE-CNT)))
    (if (not nc)
        #f
        (let ((w0 (bitwise-and (arithmetic-shift nc -16) #xFF))
              (cnt (bitwise-and nc #xFF)))
          (let loop ((w w0) (left cnt) (dac #f) (pin #f))
            (if (or (= left 0) (and dac pin))
                (if (and dac pin) (list dac pin) #f)
                (let* ((caps (cdc-param cdc w PARAM-AUDIO-WIDGET-CAPS))
                       (type (if caps (bitwise-and (arithmetic-shift caps -20) #xF) -1)))
                  (cond
                    ((and (not dac) (= type WIDGET-AUDIO-OUTPUT))
                     (loop (+ w 1) (- left 1) w pin))
                    ((and (not pin) (= type WIDGET-PIN-COMPLEX)
                          (let ((pc (cdc-param cdc w PARAM-PIN-CAPS)))
                            (and pc (not (= 0 (bitwise-and pc PIN-CAP-OUTPUT))))))
                     (loop (+ w 1) (- left 1) dac w))
                    (else (loop (+ w 1) (- left 1) dac pin))))))))))

;; Find an output path under the codec root: scan its function groups for an audio
;; FG, then find a DAC + output pin inside it. Returns (list afg dac pin) or #f.
(define (find-output-path cdc)
  (let ((nc (cdc-param cdc 0 PARAM-NODE-CNT)))
    (if (not nc)
        #f
        (let ((fg0 (bitwise-and (arithmetic-shift nc -16) #xFF))
              (cnt (bitwise-and nc #xFF)))
          (let loop ((fg fg0) (left cnt))
            (if (= left 0)
                #f
                (let ((gt (cdc-param cdc fg PARAM-FUNC-GRP-TYPE)))
                  (if (and gt (= (bitwise-and gt #x7F) 1))   ; audio function group
                      (let ((dp (find-dac-pin cdc fg)))
                        (if dp (list fg (car dp) (cadr dp))
                            (loop (+ fg 1) (- left 1))))
                      (loop (+ fg 1) (- left 1))))))))))

;; Return the index of `target` in `node`'s (short-form) connection list, or #f if
;; not found (distinct from a valid index 0 -- the caller skips SET_CONN_SELECT
;; when the DAC is not directly listed rather than mis-routing to entry 0). Each
;; GET_CONNECTION_LIST response packs four 8-bit entries; a verb timeout (#f resp)
;; skips that batch instead of crashing on a non-numeric shift. Long-form lists
;; (conn-list-len bit7, 16-bit entries) appear only on codecs with >127 widgets,
;; which neither QEMU nor common hardware has; flag rather than mis-parse them.
(define (conn-index cdc node target)
  (let* ((cl  (cdc-param cdc node PARAM-CONN-LIST-LEN))
         (len (if cl (bitwise-and cl #x7F) 0)))
    (if (and cl (not (= 0 (bitwise-and cl #x80))))
        (begin (display "[hdaudio] warning: long-form connection list unsupported")
               (newline)))
    (let loop ((i 0))
      (if (>= i len)
          #f
          (let ((resp (cdc-verb cdc node (v12 VERB-GET-CONN-LIST-ENTRY i))))
            (if (not resp)
                (loop (+ i 4))                ; verb timeout: skip this batch
                (let scan ((k 0))
                  (if (or (= k 4) (>= (+ i k) len))
                      (loop (+ i 4))
                      (if (= target (bitwise-and (arithmetic-shift resp (* k -8)) #xFF))
                          (+ i k)
                          (scan (+ k 1)))))))))))

;; --- output-path configuration ----------------------------------------------
;; The stream/format we drive: 48 kHz, 16-bit, stereo. SDFMT and the converter
;; format must agree (HDA spec 7.3.3.8 format word: base=0(48k) mult=0 div=0
;; bits=001(16) chan=0001(2ch) -> 0x0011).
(define STREAM-NUM        1)
(define FMT-48K-16-STEREO #x0011)

;; Power the DAC + pin to D0, program the DAC's format/stream/amp, route the pin to
;; the DAC and enable its output (pin control + amp + EAPD). Gains are set to the
;; DAC's advertised max so the stream is audible without clipping the device range.
(define (configure-output! cdc afg dac pin)
  (let* ((oac  (cdc-param cdc dac PARAM-OUTPUT-AMP-CAPS))
         (gain (let ((g (if oac (bitwise-and (arithmetic-shift oac -8) #x7F) 0)))
                 (if (> g 0) g #x4B))))           ; fall back to a sane mid gain
    ;; power up the whole path
    (cdc-verb cdc afg (v12 VERB-SET-POWER-STATE 0))
    (cdc-verb cdc dac (v12 VERB-SET-POWER-STATE 0))
    (cdc-verb cdc pin (v12 VERB-SET-POWER-STATE 0))
    ;; DAC: format, stream #STREAM-NUM channel 0, unmute output amp (L+R)
    (cdc-verb cdc dac (v4  VERB-SET-CONVERTER-FORMAT FMT-48K-16-STEREO))
    (cdc-verb cdc dac (v12 VERB-SET-STREAM-CHANNEL (arithmetic-shift STREAM-NUM 4)))
    (cdc-verb cdc dac (v4  VERB-SET-AMP-GAIN-MUTE (bitwise-or #xB000 gain)))
    ;; Pin: route it to the DAC (only if the DAC is directly in its connection
    ;; list -- otherwise leave the selector alone rather than mis-route to entry 0),
    ;; enable output drive (bit6) + headphone amp (bit7), unmute the pin's own
    ;; output amp, enable EAPD (external amp / not-muted line).
    (let ((ci (conn-index cdc pin dac)))
      (if ci (cdc-verb cdc pin (v12 VERB-SET-CONN-SELECT ci))))
    (cdc-verb cdc pin (v12 VERB-SET-PIN-CONTROL #xC0))
    (cdc-verb cdc pin (v4  VERB-SET-AMP-GAIN-MUTE (bitwise-or #xB000 gain)))
    (cdc-verb cdc pin (v12 VERB-SET-EAPD-BTL #x02))))

;; --- output stream descriptor + BDL ------------------------------------------
;; The HDA output stream descriptors live in MMIO after the input ones: SD base =
;; 0x80 + iss*0x20, where iss (GCAP bits8-11) is the input-stream count. We always
;; drive the FIRST output stream descriptor.
(define SD-CTL  #x00)   ; byte0: 0 SRST, 1 RUN, 2 IOCE; byte2 bits4-7 = stream #
(define SD-STS  #x03)   ; W1C: bit2 BCIS, bit3 FIFOE, bit4 DESE
(define SD-CBL  #x08)   ; u32: cyclic buffer length (bytes)
(define SD-LVI  #x0C)   ; u16: last valid BDL index
(define SD-FMT  #x12)   ; u16: stream format (== the converter format)
(define SD-BDPL #x18)   ; u32: BDL base lower
(define SD-BDPU #x1C)   ; u32: BDL base upper

(define (out-sd-base regs)
  (let ((iss (bitwise-and (arithmetic-shift (r16 regs GCAP) -8) #xF)))
    (+ #x80 (* iss #x20))))

;; Write one 16-byte BDL entry {u64 addr; u32 len; u32 ioc} at byte offset `eoff`.
(define (bdl-entry! bdl eoff phys len)
  (bytes-u32-set! bdl eoff        (bitwise-and phys #xFFFFFFFF))
  (bytes-u32-set! bdl (+ eoff 4)  (arithmetic-shift phys -32))
  (bytes-u32-set! bdl (+ eoff 8)  len)
  (bytes-u32-set! bdl (+ eoff 12) 1))           ; IOC

;; Point output stream 0 at `buf` (total-bytes of PCM) via a two-entry BDL (the
;; spec wants >= 2 entries) and run it. The stream engine cycles the BDL forever
;; (wrapping at SDCBL), so a short buffer plays as a continuous loop -- exactly
;; what a steady tone wants. Mirrors the SRST handshake every HDA stream needs.
(define (stream-run! regs sd bdl buf total-bytes)
  (let* ((bphys (bytes-phys buf))
         (half  (quotient total-bytes 2)))
    (bdl-entry! bdl #x00 bphys half)
    (bdl-entry! bdl #x10 (+ bphys half) (- total-bytes half))
    ;; reset the stream descriptor (SRST 1 -> wait -> 0 -> wait)
    (w8! regs (+ sd SD-CTL) #x01)
    (wait-until (lambda () (not (= 0 (bitwise-and (r8 regs (+ sd SD-CTL)) #x01)))) 1000000)
    (w8! regs (+ sd SD-CTL) #x00)
    (wait-until (lambda () (= 0 (bitwise-and (r8 regs (+ sd SD-CTL)) #x01))) 1000000)
    ;; clear sticky status, program BDL base / length / last-index / format
    (w8! regs (+ sd SD-STS) #x1C)
    (let ((bp (bytes-phys bdl)))
      (w32! regs (+ sd SD-BDPL) (bitwise-and bp #xFFFFFFFF))
      (w32! regs (+ sd SD-BDPU) (arithmetic-shift bp -32)))
    (w32! regs (+ sd SD-CBL) total-bytes)
    (w16! regs (+ sd SD-LVI) 1)             ; two BDL entries -> last index 1
    (w16! regs (+ sd SD-FMT) FMT-48K-16-STEREO)
    (w8!  regs (+ sd (+ SD-CTL 2)) (arithmetic-shift STREAM-NUM 4))   ; stream #
    (w8!  regs (+ sd SD-CTL) (bitwise-or (r8 regs (+ sd SD-CTL)) #x02))))  ; RUN

;; --- tone synthesis ----------------------------------------------------------
;; No FP trig primitive exists, so synthesize a square wave with integer math:
;; the sign flips every half period. Stereo 16-bit; each frame is L,R (identical).
;; A negative sample is written as its u16 two's-complement. At 48 kHz the default
;; tone loops cleanly (period divides the frame count).
(define SAMPLE-RATE 48000)
(define TONE-HZ     500)
(define TONE-FRAMES 4800)     ; 0.1 s; 96-frame period * 50 = clean loop
(define TONE-AMP    8000)     ; ~1/4 full scale -- clearly audible, not harsh

(define (fill-tone! buf frames freq amp)
  (let ((half (quotient SAMPLE-RATE (* 2 freq)))   ; frames per half period
        (neg  (- 65536 amp)))                      ; -amp as u16
    (let loop ((i 0))
      (if (< i frames)
          (let ((v   (if (= 0 (modulo (quotient i half) 2)) amp neg))
                (off (* i 4)))
            (bytes-u16-set! buf off v)              ; left
            (bytes-u16-set! buf (+ off 2) v)        ; right
            (loop (+ i 1)))
          buf))))

;; Synthesize and start a tone on output stream 0. Returns (list buf bdl) -- the
;; caller MUST retain these: the stream DMAs `buf` forever, so if it were GC'd and
;; its pages reused the audio would corrupt. nframes frames -> nframes*4 bytes.
(define (play-tone! regs freq amp nframes)
  (let* ((nbytes (* nframes 4))
         (buf (dma-alloc-32 nbytes))
         (bdl (dma-alloc-32 256)))            ; 16 BDL entries' worth (we use 2)
    (fill-tone! buf nframes freq amp)
    (stream-run! regs (out-sd-base regs) bdl buf nbytes)
    (display "[hdaudio] playing ") (display freq) (display "Hz tone on stream ")
    (display STREAM-NUM) (newline)
    (list buf bdl)))

;; --- the long-lived driver context -------------------------------------------
;; After bring-up the spawned context becomes the audio card's service loop. It
;; retains the currently-playing buffers (so the stream's DMA source stays live)
;; and answers coreaudio: (tone) replays the default tone, (play freq amp frames)
;; plays an arbitrary square-wave tone, (get-status reply) reports state. All tone
;; parameters are plain fixnums, so nothing crosses the context boundary except
;; small values -- no shared DMA buffer between contexts is needed.
(define (hda-driver-loop regs refs)
  (let loop ((cur refs))
    (let ((m (recv)))
      (cond
        ((eq? (car m) 'tone)
         (loop (play-tone! regs TONE-HZ TONE-AMP TONE-FRAMES)))
        ((eq? (car m) 'play)            ; (play freq amp frames)
         ;; Validate before synthesis: freq 0 divides by zero in fill-tone!, amp
         ;; outside [1,32767] overflows the signed-16 sample, frames 0 writes
         ;; CBL=0 (an undefined running stream). A bad request is dropped, not fatal.
         (let ((freq (nth m 1)) (amp (nth m 2)) (frames (nth m 3)))
           (if (and (> freq 0) (> amp 0) (< amp 32768) (> frames 0))
               (loop (play-tone! regs freq amp frames))
               (begin (display "[hdaudio] play: bad params, ignored") (newline)
                      (loop cur)))))
        ((eq? (car m) 'get-status)
         (send (nth m 1) 'playing) (loop cur))
        (else (loop cur))))))

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
                     ;; still register the (empty) card so the endpoint exists, and
                     ;; park on recv (the card ctx with no playable path).
                     (send audio (list 'register 'hda0 (self)))
                     (let loop () (recv) (loop)))
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
                    ;; Find an output path on the first present codec, configure it,
                    ;; and play the bring-up tone -- the live proof that a guest
                    ;; stream reaches the codec (the C driver stopped at enumeration).
                    (let* ((addr (first-codec-addr statests))
                           (cdc  (make-cdc regs ring rirb-off slot-cnt st addr))
                           (path (find-output-path cdc)))
                      (send audio (list 'register 'hda0 (self)))
                      (display "[hdaudio] registered hda0 with coreaudio") (newline)
                      (if (not path)
                          (begin
                            (display "[hdaudio] no output path on codec ") (display addr)
                            (display "; card idle") (newline)
                            (let loop () (recv) (loop)))
                          (let ((afg (nth path 0)) (dac (nth path 1)) (pin (nth path 2)))
                            (display "[hdaudio] output path codec=") (display addr)
                            (display " afg=") (display afg) (display " dac=") (display dac)
                            (display " pin=") (display pin) (newline)
                            (configure-output! cdc afg dac pin)
                            ;; play the startup tone, then serve the card forever
                            ;; (retaining the playing buffers so DMA stays valid).
                            (hda-driver-loop
                             regs (play-tone! regs TONE-HZ TONE-AMP TONE-FRAMES)))))))))))))

;; --- entry point -------------------------------------------------------------
;; hdaudio-init takes the coreaudio service handle. Gated on pci-find-class so a
;; no-audio boot just logs + returns.
(define (find-hda)
  (pci-find-class HDA-CLASS HDA-SUBCLASS))

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
                ;; Map a full page: the global regs end at 0x180 only for ISS=OSS=4
                ;; (ICH9/ICH6); a controller with more streams puts output stream
                ;; descriptors past 0x180 (out-sd-base = 0x80 + iss*0x20), so size the
                ;; window to the page the BAR already occupies rather than 0x180.
                (let ((regs (mmio-map bar-phys #x1000)))
                  ;; Run reset + ring setup + enumeration in a spawned context so the
                  ;; reset/settle waits yield (same pattern as ahci-init). Fire and
                  ;; forget: the context logs + registers and reports to the log;
                  ;; init does not await it.
                  (spawn-restricted '()
                    (lambda () (hdaudio-bringup regs ecam audio)))
                  'hdaudio-spawned))))))) ) ; last ) closes (define-module hdaudio ...)
