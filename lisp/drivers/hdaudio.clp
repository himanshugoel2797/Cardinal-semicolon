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
  ;; hdaudio-init is the entry point; the rest are pure graph/classification
  ;; helpers exported so the in-OS SysTest can drive enumerate-endpoints with a
  ;; MOCK codec (a (node payload)->response lambda) and check the classified
  ;; endpoint set without real hardware -- the same testable-internals posture as
  ;; the rtl8169 driver's exported descriptor helpers.
  (export hdaudio-init enumerate-endpoints endpoint-descs primary-output ep-desc
          set-endpoint-volume! set-endpoint-mute! ep-vol configure-input!
          poll-jacks! read-present)
  (import sys-mmio sys-pci driver-util)
  (define lg (make-logger 'hdaudio))

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

;; HDA GET_PARAMETER parameter ids (subset, from inc/cmds.h)
(define PARAM-VENDOR-DEVICE-ID #x00)
(define PARAM-NODE-CNT         #x04)   ; subordinate node count: (start<<16)|count
(define PARAM-FUNC-GRP-TYPE    #x05)   ; low 7 bits: 1 = audio function group
(define PARAM-AUDIO-WIDGET-CAPS #x09)  ; bits20-23 = widget type
(define PARAM-PIN-CAPS         #x0C)   ; bit4 = output-capable, bit5 = input-capable
(define PARAM-CONN-LIST-LEN    #x0E)   ; bits0-6 = length, bit7 = long-form
(define PARAM-OUTPUT-AMP-CAPS  #x12)   ; bits8-14 = num-steps (max gain)
(define PARAM-INPUT-AMP-CAPS   #x0D)   ; like output-amp-caps, for input amps

;; widget types (audio-widget-caps bits20-23)
(define WIDGET-AUDIO-OUTPUT 0)   ; a DAC (converter: stream -> analog)
(define WIDGET-AUDIO-INPUT  1)   ; an ADC (converter: analog -> stream)
(define WIDGET-MIXER        2)   ; an amplifier/mixer (sums its connection list)
(define WIDGET-SELECTOR     3)   ; a selector/mux (picks one connection-list entry)
(define WIDGET-PIN-COMPLEX  4)   ; a jack/pin
(define PIN-CAP-OUTPUT      #x10) ; pin-caps bit4: pin can drive an output
(define PIN-CAP-INPUT       #x20) ; pin-caps bit5: pin can sense an input

;; --- codec handle + verb builders -------------------------------------------
;; A "codec handle" is just a FUNCTION (node payload) -> response: it closes over
;; everything hda-verb! needs except the target node and the verb payload, so the
;; graph-discovery / configuration code can talk to a codec without threading six
;; arguments through every call. Making it a closure (rather than a list of the six
;; values) also decouples the graph walk from the MMIO transport: a test can drive
;; enumerate-endpoints with a mock responder lambda instead of real hardware.
(define (make-cdc regs ring rirb-off slot-cnt st addr)
  (lambda (node payload) (hda-verb! regs ring rirb-off slot-cnt st addr node payload)))
(define (cdc-verb cdc node payload) (cdc node payload))
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
(define VERB-GET-CONFIG-DEFAULT   #xF1C)    ; 12-bit; -> the pin's 32-bit config default
(define VERB-GET-PIN-SENSE        #xF09)    ; 12-bit; bit31 = presence detected

;; --- codec graph walk --------------------------------------------------------
;; The C driver enumerated every widget but never used the graph (the path loops
;; are commented out). To drive speakers, headphones AND mics we walk the whole
;; audio function group: classify every pin complex by its CONFIGURATION DEFAULT
;; (which the BIOS/codec fills in with the jack's real device type) plus its in/out
;; caps, and resolve each usable pin to a converter (DAC for output, ADC for input)
;; by following connection lists through any mixers/selectors. The result is a list
;; of ENDPOINTS -- the speaker, the headphone jack, the mic, the line-in -- each of
;; which the service can configure, route a stream to, and set a volume on.

;; (start . count) of a node's subordinate widgets, or #f.
(define (node-children cdc nid)
  (let ((nc (cdc-param cdc nid PARAM-NODE-CNT)))
    (if nc (cons (bitwise-and (arithmetic-shift nc -16) #xFF) (bitwise-and nc #xFF)) #f)))

;; A widget's type (audio-widget-caps bits20-23), or -1 if the verb times out.
(define (widget-type cdc nid)
  (let ((caps (cdc-param cdc nid PARAM-AUDIO-WIDGET-CAPS)))
    (if caps (bitwise-and (arithmetic-shift caps -20) #xF) -1)))

;; A widget's (short-form) connection list as a list of node ids. Each
;; GET_CONNECTION_LIST response packs four 8-bit entries; a verb timeout (#f resp)
;; skips that batch. Long-form lists (conn-list-len bit7, 16-bit entries) appear
;; only on codecs with >127 widgets, which neither QEMU nor common hardware has;
;; flag rather than mis-parse them.
(define (conn-entries cdc node)
  (let* ((cl  (cdc-param cdc node PARAM-CONN-LIST-LEN))
         (len (if cl (bitwise-and cl #x7F) 0)))
    (if (and cl (not (= 0 (bitwise-and cl #x80))))
        ;; long-form (16-bit entries): bail rather than misparse them as 8-bit --
        ;; falling through would yield garbage NIDs and silently break the walk.
        (begin (lg "warning: long-form connection list unsupported")
               '())
        (let loop ((i 0) (acc '()))
          (if (>= i len)
              (reverse acc)
              (let ((resp (cdc-verb cdc node (v12 VERB-GET-CONN-LIST-ENTRY i))))
                (if (not resp)
                    (loop (+ i 4) acc)            ; verb timeout: skip this batch
                    (let scan ((k 0) (acc acc))
                      (if (or (= k 4) (>= (+ i k) len))
                          (loop (+ i 4) acc)
                          (scan (+ k 1)
                                (cons (bitwise-and (arithmetic-shift resp (* k -8)) #xFF)
                                      acc)))))))))))

;; Index of `target` in `node`'s connection list (for SET_CONN_SELECT), or #f.
(define (conn-index cdc node target)
  (let loop ((es (conn-entries cdc node)) (i 0))
    (cond ((null? es) #f)
          ((= (car es) target) i)
          (else (loop (cdr es) (+ i 1))))))

;; Follow `node`'s connection list (through mixers/selectors, bounded depth) to the
;; first converter of `want-type` (DAC for output, ADC for input). Returns the
;; converter nid or #f. Bounded depth guards against a cyclic/odd graph. The
;; per-entry scan is a SEPARATE top-level function (find-conv is called from it as
;; a sibling global): the bytecode VM cannot capture a top-level define's own name
;; from inside its own nested loop, so recursion must go sibling<->sibling, not
;; self-from-a-named-let.
(define (find-conv cdc node want-type depth)
  (if (<= depth 0) #f (find-conv-list cdc (conn-entries cdc node) want-type depth)))
(define (find-conv-list cdc es want-type depth)
  (if (null? es)
      #f
      (let ((ty (widget-type cdc (car es))))
        (cond
          ((= ty want-type) (car es))
          ((or (= ty WIDGET-MIXER) (= ty WIDGET-SELECTOR))
           (let ((r (find-conv cdc (car es) want-type (- depth 1))))
             (if r r (find-conv-list cdc (cdr es) want-type depth))))
          (else (find-conv-list cdc (cdr es) want-type depth))))))

;; Does `node` reach `target` through its connection graph (bounded)? Used to find
;; the ADC that an input pin feeds: ADCs list their input pins, so we look for an
;; ADC whose graph reaches the pin. Same sibling-recursion split as find-conv.
(define (reaches? cdc node target depth)
  (if (<= depth 0) #f (reaches-list? cdc (conn-entries cdc node) target depth)))
(define (reaches-list? cdc es target depth)
  (if (null? es)
      #f
      (let ((ty (widget-type cdc (car es))))
        (cond
          ((= (car es) target) #t)
          ((or (= ty WIDGET-MIXER) (= ty WIDGET-SELECTOR))
           (if (reaches? cdc (car es) target (- depth 1)) #t
               (reaches-list? cdc (cdr es) target depth)))
          (else (reaches-list? cdc (cdr es) target depth))))))

;; Max links to follow between a pin and its converter: a pin -> (up to 3
;; mixers/selectors) -> converter covers every QEMU and consumer codec graph.
(define GRAPH-DEPTH 4)

;; Resolve an output pin to a DAC by following its connection list.
(define (resolve-output-conv cdc pin) (find-conv cdc pin WIDGET-AUDIO-OUTPUT GRAPH-DEPTH))

;; Resolve an input pin to an ADC: scan the AFG's widgets for an ADC that reaches
;; the pin. (afg-start . afg-cnt) bounds the scan.
(define (resolve-input-conv cdc afg-start afg-cnt pin)
  (let loop ((w afg-start) (left afg-cnt))
    (cond ((= left 0) #f)
          ((and (= (widget-type cdc w) WIDGET-AUDIO-INPUT)
                (reaches? cdc w pin GRAPH-DEPTH)) w)
          (else (loop (+ w 1) (- left 1))))))

;; Find the first audio function group under the codec root, or #f.
(define (find-afg cdc)
  (let ((ch (node-children cdc 0)))
    (if (not ch)
        #f
        (let loop ((fg (car ch)) (left (cdr ch)))
          (if (= left 0)
              #f
              (let ((gt (cdc-param cdc fg PARAM-FUNC-GRP-TYPE)))
                (if (and gt (= (bitwise-and gt #x7F) 1))   ; audio function group
                    fg
                    (loop (+ fg 1) (- left 1)))))))))

;; --- pin configuration default decode ----------------------------------------
;; GET_CONFIG_DEFAULT returns a 32-bit word the platform fills in describing what
;; is wired to the pin. We use two fields: the default DEVICE (what kind of jack)
;; and PORT CONNECTIVITY (whether anything is physically attached at all).
(define (cfg-device cfg)       (bitwise-and (arithmetic-shift cfg -20) #xF))
(define (cfg-connectivity cfg) (bitwise-and (arithmetic-shift cfg -30) #x3))
(define CONN-NONE 1)   ; port connectivity 1 = no physical connection (dead pin)

;; Is something plugged into this pin? GET_PIN_SENSE bit31 = presence detected. A
;; verb timeout (or a pin without sense) is treated as present (#t) so a flaky read
;; never spuriously marks a working jack absent. Read at enumeration AND by the
;; jack-detect poller to notice plug/unplug. NOTE: a jack that uses impedance/
;; resistance sensing needs an EXECUTE_PIN_SENSE (0xF08) trigger before the value is
;; fresh; we issue only GET_PIN_SENSE (correct for QEMU + voltage-presence jacks,
;; which is the common case). Add the trigger if a resistance-sense codec turns up.
(define (read-present cdc pin)
  (let ((s (cdc-verb cdc pin (v12 VERB-GET-PIN-SENSE 0))))
    (if s (not (= 0 (bitwise-and s #x80000000))) #t)))

;; Default-device code -> a friendly endpoint symbol.
(define (dev-name code)
  (cond ((= code #x0) 'line-out)  ((= code #x1) 'speaker)   ((= code #x2) 'headphone)
        ((= code #x4) 'spdif-out) ((= code #x5) 'digital-out)
        ((= code #x8) 'line-in)   ((= code #x9) 'aux)       ((= code #xA) 'mic)
        ((= code #xC) 'spdif-in)  ((= code #xD) 'digital-in)
        (else 'other)))

;; --- endpoint records --------------------------------------------------------
;; An endpoint is a mutable vector so later passes (PR2 volume, PR4 hotplug) can
;; update its volume/present fields in place. Built once per codec by
;; enumerate-endpoints. `conv` is the DAC (output) or ADC (input) the pin routes to.
;; Field 8 is the endpoint's codec handle (the verb closure) -- each endpoint
;; carries how to talk to ITS codec, so per-endpoint ops (volume, capture) need no
;; ambient cdc, and a card spanning several codecs (or one hot-added later) just
;; works. The cdc is a closure, so it is NOT part of ep-desc (which crosses context
;; boundaries); only fixnums/symbols cross.
(define EP-LEN 9)
(define (mk-endpoint id dir dev pin conv afg present cdc)
  (let ((e (make-vector EP-LEN 0)))
    (vector-set! e 0 id)   (vector-set! e 1 dir)  (vector-set! e 2 dev)
    (vector-set! e 3 pin)  (vector-set! e 4 conv) (vector-set! e 5 afg)
    (vector-set! e 6 present) (vector-set! e 7 100)   ; default volume 100%
    (vector-set! e 8 cdc)
    e))
(define (ep-id e)      (vector-ref e 0))
(define (ep-dir e)     (vector-ref e 1))   ; 'out | 'in
(define (ep-dev e)     (vector-ref e 2))   ; 'speaker 'headphone 'line-out 'mic ...
(define (ep-pin e)     (vector-ref e 3))
(define (ep-conv e)    (vector-ref e 4))
(define (ep-afg e)     (vector-ref e 5))
(define (ep-present e) (vector-ref e 6))
(define (ep-present! e v) (vector-set! e 6 v))
(define (ep-vol e)     (vector-ref e 7))
(define (ep-vol! e v)  (vector-set! e 7 v))
(define (ep-cdc e)     (vector-ref e 8))

;; Walk an AFG's pin complexes, classify each by config-default + caps, resolve its
;; converter, and build the endpoint list. A pin with no physical connection, or
;; that resolves to no converter, is skipped (unusable). A combo jack that is both
;; in- and out-capable yields one endpoint per direction.
;; `start-id` is the first endpoint id to assign, so endpoints from several codecs
;; on one card stay globally unique (each codec's scan continues the numbering).
(define (enumerate-endpoints cdc start-id)
  (let ((afg (find-afg cdc)))
    (if (not afg)
        '()
        (let ((ch (node-children cdc afg)))
          (if (not ch)
              '()
              (let ((w0 (car ch)) (cnt (cdr ch)))
                (let loop ((w w0) (left cnt) (id start-id) (acc '()))
                  (if (= left 0)
                      (reverse acc)
                      (if (not (= (widget-type cdc w) WIDGET-PIN-COMPLEX))
                          (loop (+ w 1) (- left 1) id acc)
                          (let* ((pc  (cdc-param cdc w PARAM-PIN-CAPS))
                                 (cfg (cdc-verb cdc w (v12 VERB-GET-CONFIG-DEFAULT 0)))
                                 (dev (dev-name (if cfg (cfg-device cfg) #xF)))
                                 (dead (and cfg (= (cfg-connectivity cfg) CONN-NONE))))
                            (if (or (not pc) dead)
                                (loop (+ w 1) (- left 1) id acc)
                                ;; only a usable pin gets the extra pin-sense verb
                                (let* ((present (read-present cdc w))
                                       (out? (not (= 0 (bitwise-and pc PIN-CAP-OUTPUT))))
                                       (in?  (not (= 0 (bitwise-and pc PIN-CAP-INPUT))))
                                       (oc (if out? (resolve-output-conv cdc w) #f))
                                       (ic (if in?  (resolve-input-conv cdc w0 cnt w) #f))
                                       (acc (if (and out? oc)
                                                (cons (mk-endpoint id 'out dev w oc afg present cdc) acc)
                                                acc))
                                       (id  (if (and out? oc) (+ id 1) id))
                                       (acc (if (and in? ic)
                                                (cons (mk-endpoint id 'in dev w ic afg present cdc) acc)
                                                acc))
                                       (id  (if (and in? ic) (+ id 1) id)))
                                  (loop (+ w 1) (- left 1) id acc)))))))))))))

;; --- output-path configuration ----------------------------------------------
;; The stream/format we drive: 48 kHz, 16-bit, stereo. SDFMT and the converter
;; format must agree (HDA spec 7.3.3.8 format word: base=0(48k) mult=0 div=0
;; bits=001(16) chan=0001(2ch) -> 0x0011).
(define STREAM-NUM        1)
(define FMT-48K-16-STEREO #x0011)

;; A converter/pin's max amp gain (num-steps, amp-caps bits8-14), or a sane mid
;; fallback if the widget advertises none. The gain field of SET_AMP_GAIN_MUTE is
;; 7 bits; this is the value that maps to 100% volume.
(define (amp-max-gain cdc nid param)
  (let* ((ac (cdc-param cdc nid param))
         (g (if ac (bitwise-and (arithmetic-shift ac -8) #x7F) 0)))
    (if (> g 0) g #x4B)))

;; SET_AMP_GAIN_MUTE payload: set output(bit15)/input(bit14) amp, both channels
;; (left bit13 + right bit12), mute bit7, gain bits0-6. The output form sets only
;; bit15; the input form (for capture gain) sets only bit14.
(define (amp-out-payload gain mute) (bitwise-or #xB000 (if mute #x80 0) (bitwise-and gain #x7F)))
(define (amp-in-payload  gain mute) (bitwise-or #x7000 (if mute #x80 0) (bitwise-and gain #x7F)))

;; --- per-endpoint volume -----------------------------------------------------
;; Volume is a 0..100 percentage mapped onto the target amp's gain range. The
;; target is the PIN's own amp when it has one (so a speaker and a headphone jack
;; sharing one DAC get INDEPENDENT volume); otherwise the converter's amp (shared,
;; but the only knob available). Output endpoints drive the output amp, input
;; endpoints the input (capture) amp. vol 0 mutes.

;; widget-caps bit2 = out-amp present, bit1 = in-amp present.
(define (widget-has-amp? cdc nid dir)
  (let ((caps (cdc-param cdc nid PARAM-AUDIO-WIDGET-CAPS)))
    (and caps (not (= 0 (bitwise-and caps (if (eq? dir 'out) #x4 #x2)))))))

;; The GET_PARAMETER amp-caps id for an endpoint's direction.
(define (amp-cap-param dir) (if (eq? dir 'out) PARAM-OUTPUT-AMP-CAPS PARAM-INPUT-AMP-CAPS))

;; The node whose amp carries this endpoint's volume: the pin if it has the right
;; amp, else the converter.
(define (ep-amp-target ep)
  (if (widget-has-amp? (ep-cdc ep) (ep-pin ep) (ep-dir ep)) (ep-pin ep) (ep-conv ep)))

;; Issue SET_AMP_GAIN_MUTE for a direction.
(define (apply-amp! cdc nid dir gain mute)
  (cdc-verb cdc nid (v4 VERB-SET-AMP-GAIN-MUTE
                        (if (eq? dir 'out) (amp-out-payload gain mute)
                            (amp-in-payload gain mute)))))

;; clamp v to 0..100
(define (clamp-vol v) (cond ((< v 0) 0) ((> v 100) 100) (else v)))

;; Set an endpoint's volume (0..100): map onto the target amp's step range and
;; program it (mute at 0), then remember it on the endpoint. Returns the clamped
;; volume actually applied. Uses the endpoint's own codec handle.
(define (set-endpoint-volume! ep vol)
  (let* ((cdc   (ep-cdc ep))
         (dir   (ep-dir ep))
         (nid   (ep-amp-target ep))
         (steps (amp-max-gain cdc nid (amp-cap-param dir)))
         (v     (clamp-vol vol))
         (gain  (quotient (* v steps) 100)))
    (apply-amp! cdc nid dir gain (= v 0))
    (ep-vol! ep v)
    v))

;; Mute/unmute without losing the stored volume: re-issue the amp at the endpoint's
;; current volume gain, with the mute bit set/cleared.
(define (set-endpoint-mute! ep on?)
  (let* ((cdc   (ep-cdc ep))
         (dir   (ep-dir ep))
         (nid   (ep-amp-target ep))
         (steps (amp-max-gain cdc nid (amp-cap-param dir)))
         (gain  (quotient (* (ep-vol ep) steps) 100)))
    (apply-amp! cdc nid dir gain on?)))

;; Find an endpoint by its id within a card's endpoint list, or #f.
(define (ep-by-id eps id)
  (cond ((null? eps) #f)
        ((= (ep-id (car eps)) id) (car eps))
        (else (ep-by-id (cdr eps) id))))

;; Power the converter + pin to D0, program the DAC's format/stream, route the pin
;; to the DAC and enable its output (pin control + amp + EAPD). The output amps are
;; opened to the converter's advertised max so the stream is audible without
;; clipping; PR2 layers per-endpoint volume on top by re-issuing SET_AMP_GAIN_MUTE.
(define (configure-output! ep)
  (let* ((cdc (ep-cdc ep)) (afg (ep-afg ep)) (dac (ep-conv ep)) (pin (ep-pin ep))
         (gain (amp-max-gain cdc dac PARAM-OUTPUT-AMP-CAPS)))
    ;; power up the whole path
    (cdc-verb cdc afg (v12 VERB-SET-POWER-STATE 0))
    (cdc-verb cdc dac (v12 VERB-SET-POWER-STATE 0))
    (cdc-verb cdc pin (v12 VERB-SET-POWER-STATE 0))
    ;; DAC: format, stream #STREAM-NUM channel 0, unmute output amp (L+R)
    (cdc-verb cdc dac (v4  VERB-SET-CONVERTER-FORMAT FMT-48K-16-STEREO))
    (cdc-verb cdc dac (v12 VERB-SET-STREAM-CHANNEL (arithmetic-shift STREAM-NUM 4)))
    (cdc-verb cdc dac (v4  VERB-SET-AMP-GAIN-MUTE (amp-out-payload gain #f)))
    ;; Pin: route it to the DAC (only if the DAC is in its connection list --
    ;; otherwise leave the selector alone rather than mis-route to entry 0), enable
    ;; output drive (bit6) + headphone amp (bit7), unmute the pin's own output amp,
    ;; enable EAPD (external amp / not-muted line).
    (let ((ci (conn-index cdc pin dac)))
      (if ci (cdc-verb cdc pin (v12 VERB-SET-CONN-SELECT ci))))
    (cdc-verb cdc pin (v12 VERB-SET-PIN-CONTROL #xC0))
    (cdc-verb cdc pin (v4  VERB-SET-AMP-GAIN-MUTE (amp-out-payload gain #f)))
    (cdc-verb cdc pin (v12 VERB-SET-EAPD-BTL #x02))))

;; --- output stream descriptor + BDL ------------------------------------------
;; The HDA output stream descriptors live in MMIO after the input ones: SD base =
;; 0x80 + iss*0x20, where iss (GCAP bits8-11) is the input-stream count. We always
;; drive the FIRST output stream descriptor.
(define SD-CTL  #x00)   ; byte0: 0 SRST, 1 RUN, 2 IOCE; byte2 bits4-7 = stream #
(define SD-STS  #x03)   ; W1C: bit2 BCIS, bit3 FIFOE, bit4 DESE
(define SD-LPIB #x04)   ; u32: link position in buffer (DMA progress, bytes)
(define SD-CBL  #x08)   ; u32: cyclic buffer length (bytes)
(define SD-LVI  #x0C)   ; u16: last valid BDL index
(define SD-FMT  #x12)   ; u16: stream format (== the converter format)
(define SD-BDPL #x18)   ; u32: BDL base lower
(define SD-BDPU #x1C)   ; u32: BDL base upper

;; Stream descriptors are laid out input-streams-first: input stream n at
;; 0x80 + n*0x20, output stream n at 0x80 + (iss+n)*0x20, where iss (GCAP
;; bits8-11) is the input-stream count. We drive the first of each.
(define (iss-of regs) (bitwise-and (arithmetic-shift (r16 regs GCAP) -8) #xF))
(define (in-sd-base regs)  #x80)                       ; input stream 0 (regs unused; symmetric with out-sd-base)
(define (out-sd-base regs) (+ #x80 (* (iss-of regs) #x20)))   ; output stream 0

;; Write one 16-byte BDL entry {u64 addr; u32 len; u32 ioc} at byte offset `eoff`.
(define (bdl-entry! bdl eoff phys len)
  (bytes-u32-set! bdl eoff        (bitwise-and phys #xFFFFFFFF))
  (bytes-u32-set! bdl (+ eoff 4)  (arithmetic-shift phys -32))
  (bytes-u32-set! bdl (+ eoff 8)  len)
  (bytes-u32-set! bdl (+ eoff 12) 1))           ; IOC

;; Point stream descriptor `sd` at `buf` (total-bytes) via a two-entry BDL (the
;; spec wants >= 2 entries), tag it `stream-num`, and run it. The stream engine
;; cycles the BDL forever (wrapping at SDCBL), so an output buffer plays as a
;; continuous loop and an input buffer is a continuously-overwritten capture ring.
;; Mirrors the SRST handshake every HDA stream needs. Used for both directions:
;; output passes out-sd-base + STREAM-NUM, capture passes in-sd-base + CAPTURE-STREAM.
(define (stream-run! regs sd bdl buf total-bytes stream-num)
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
    (w8!  regs (+ sd (+ SD-CTL 2)) (arithmetic-shift stream-num 4))   ; stream #
    (w8!  regs (+ sd SD-CTL) (bitwise-or (r8 regs (+ sd SD-CTL)) #x02))))  ; RUN

;; Stop a running stream descriptor: clear RUN and wait for the controller to
;; acknowledge it (RUN reads back 0). The wait matters because a stopped capture
;; stream's buffer is dropped right after; the DMA engine can finish one more
;; transfer after RUN=0, so we must see it halt before the buffer is GC-eligible.
(define (stream-stop! regs sd)
  (w8! regs (+ sd SD-CTL) (bitwise-and (r8 regs (+ sd SD-CTL)) (bitwise-not #x02)))
  (wait-until (lambda () (= 0 (bitwise-and (r8 regs (+ sd SD-CTL)) #x02))) 1000000))

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
    (stream-run! regs (out-sd-base regs) bdl buf nbytes STREAM-NUM)
    (lg "playing " freq "Hz tone on stream " STREAM-NUM)
    (list buf bdl)))

;; --- capture (input stream) --------------------------------------------------
;; Driving an input endpoint: configure the ADC + input pin, then run the FIRST
;; input stream descriptor capturing into a cyclic buffer the controller fills.
;; QEMU's hda has no input source under a `wav` audiodev, so captured PCM is
;; silence there, but the DMA engine still runs (SD-LPIB advances) -- the proof
;; the capture path is live; on real hardware the buffer holds mic/line-in audio.
(define CAPTURE-STREAM 2)        ; stream tag for capture (output uses 1)
(define CAP-FRAMES 4800)         ; 0.1 s cyclic capture ring at 48 kHz stereo16

;; Configure an input endpoint's path: power ADC + pin, enable the pin's input
;; (PIN_CONTROL IN_EN bit5), route the ADC to the pin, set the ADC's capture
;; format + stream tag, and open the input amps to the converter's max.
(define (configure-input! ep stream-num)
  (let* ((cdc (ep-cdc ep)) (afg (ep-afg ep)) (adc (ep-conv ep)) (pin (ep-pin ep))
         (gain (amp-max-gain cdc adc PARAM-INPUT-AMP-CAPS)))
    (cdc-verb cdc afg (v12 VERB-SET-POWER-STATE 0))
    (cdc-verb cdc adc (v12 VERB-SET-POWER-STATE 0))
    (cdc-verb cdc pin (v12 VERB-SET-POWER-STATE 0))
    ;; pin: enable input (IN_EN bit5), unmute the pin's input amp
    (cdc-verb cdc pin (v12 VERB-SET-PIN-CONTROL #x20))
    (cdc-verb cdc pin (v4  VERB-SET-AMP-GAIN-MUTE (amp-in-payload gain #f)))
    ;; ADC: route to the pin (if it selects among inputs), format, stream tag,
    ;; unmute the ADC input amp.
    (let ((ci (conn-index cdc adc pin)))
      (if ci (cdc-verb cdc adc (v12 VERB-SET-CONN-SELECT ci))))
    (cdc-verb cdc adc (v4  VERB-SET-CONVERTER-FORMAT FMT-48K-16-STEREO))
    (cdc-verb cdc adc (v12 VERB-SET-STREAM-CHANNEL (arithmetic-shift stream-num 4)))
    (cdc-verb cdc adc (v4  VERB-SET-AMP-GAIN-MUTE (amp-in-payload gain #f)))))

;; Start capturing on an input endpoint: configure the path, allocate the cyclic
;; ring, run input stream 0. Returns (buf bdl nbytes) -- the caller MUST retain it
;; (the controller DMAs into buf forever).
(define (capture-start! regs ep)
  (configure-input! ep CAPTURE-STREAM)
  (let* ((nbytes (* CAP-FRAMES 4))
         (buf (dma-alloc-32 nbytes))
         (bdl (dma-alloc-32 256)))
    (stream-run! regs (in-sd-base regs) bdl buf nbytes CAPTURE-STREAM)
    (lg "capturing on stream " CAPTURE-STREAM " pin=" (ep-pin ep) " adc=" (ep-conv ep))
    (list buf bdl nbytes)))

;; The capture DMA position (bytes written into the ring so far, mod CBL).
(define (capture-pos regs) (r32 regs (+ (in-sd-base regs) SD-LPIB)))

;; --- endpoint selection + reporting ------------------------------------------
;; An endpoint descriptor is the plain-data view of an endpoint that crosses the
;; context boundary to coreaudio and clients: (id dir dev present). Only fixnums
;; and symbols, so the copy-on-send is cheap and shares no mutable state.
(define (ep-desc e) (list (ep-id e) (ep-dir e) (ep-dev e) (ep-present e)))
(define (endpoint-descs eps) (map ep-desc eps))

(define (find-ep-dev eps dir dev)
  (cond ((null? eps) #f)
        ((and (eq? (ep-dir (car eps)) dir) (eq? (ep-dev (car eps)) dev)) (car eps))
        (else (find-ep-dev (cdr eps) dir dev))))
(define (first-of-dir eps dir)
  (cond ((null? eps) #f)
        ((eq? (ep-dir (car eps)) dir) (car eps))
        (else (first-of-dir (cdr eps) dir))))
;; The output endpoint a bare (play)/(tone) drives: prefer an internal speaker,
;; then a line-out, then a headphone jack, else the first output found.
(define (primary-output eps)
  (or (find-ep-dev eps 'out 'speaker)
      (find-ep-dev eps 'out 'line-out)
      (find-ep-dev eps 'out 'headphone)
      (first-of-dir eps 'out)))

(define (log-endpoints eps)
  (for-each
   (lambda (e)
     (lg "  endpoint " (ep-id e) " " (ep-dir e) " " (ep-dev e)
         " pin=" (ep-pin e) " conv=" (ep-conv e)
         (if (ep-present e) " present" " absent")))
   eps))

;; --- jack-presence detection (plug / unplug) ---------------------------------
;; Re-read every endpoint's pin sense and, for any whose presence changed since it
;; was last seen, update it in place and report (id dev present). A periodic poller
;; calls this so inserting/removing a plug is noticed (~1 s latency). We POLL rather
;; than enable codec UNSOLICITED responses: an unsolicited response would interleave
;; in the RIRB and break hda-verb!'s solicited-response poller, and QEMU's codec
;; emits no jack event for either mechanism -- so polling is the lower-risk path that
;; exercises its read side live. Returns the list of changes (for tests / callers).
(define (poll-jacks! eps)
  (let loop ((es eps) (changes '()))
    (if (null? es)
        (reverse changes)
        (let* ((e (car es))
               (np (read-present (ep-cdc e) (ep-pin e))))
          (if (eq? np (ep-present e))
              (loop (cdr es) changes)
              (begin
                (ep-present! e np)
                (lg "jack " (ep-dev e) (if np " inserted" " removed")
                    " (ep " (ep-id e) ")")
                (loop (cdr es) (cons (list (ep-id e) (ep-dev e) np) changes))))))))

(define JACK-POLL-NS 1000000000)   ; poll pin sense once a second

;; Spawn the jack-detect poller: every JACK-POLL-NS it pokes the driver loop with
;; (jack-poll), which re-reads pin sense (the verbs must run in the loop's context,
;; which owns the codec). It only sends a message -- no MMIO -- so it never races
;; the driver loop.
(define (start-jack-poller drvloop)
  (spawn-restricted '()
    (lambda ()
      (let loop ()
        (sleep JACK-POLL-NS)
        (send drvloop (list 'jack-poll))
        (loop)))))

;; --- codec scan / hotplug reconciliation -------------------------------------
;; Probe EVERY codec address (0..14) for a present codec and build the card's full
;; endpoint list: a codec answers a vendor-id verb iff present, so for each present
;; codec we enumerate its endpoints (ids continue across codecs, so they stay
;; globally unique) and configure its output endpoints. This is the single
;; reconciliation point -- run once at bring-up and again on every controller
;; state-change interrupt (codec hot-add/remove). It is idempotent: re-running it
;; after a codec appears/leaves yields the new endpoint set, and re-configuring an
;; already-configured output just re-sends the same verbs.
(define (scan-all-codecs regs ring rirb-off slot-cnt st)
  (let loop ((addr 0) (id 0) (acc '()))
    (if (= addr 15)
        acc
        (let* ((cdc (make-cdc regs ring rirb-off slot-cnt st addr))
               (vid (cdc-param cdc 0 PARAM-VENDOR-DEVICE-ID)))
          (if (or (not vid) (= vid 0))
              (loop (+ addr 1) id acc)                ; no codec at this address
              (let ((ceps (enumerate-endpoints cdc id)))
                (for-each (lambda (e) (if (eq? (ep-dir e) 'out) (configure-output! e))) ceps)
                (loop (+ addr 1) (+ id (length ceps)) (append acc ceps))))))))

;; The controller's state-change watcher: parks on the controller MSI and, whenever
;; STATESTS shows a codec SDIN changed (a codec was hot-added or removed), clears
;; the change (W1C) and tells the driver loop to re-enumerate. It only touches
;; STATESTS, never the RIRB/verb registers the driver loop polls, so the two
;; contexts don't race. Most MSIs are ordinary RIRB-response interrupts; for those
;; STATESTS reads 0 and the watcher does nothing.
;;
;; It checks STATESTS at the TOP of each iteration -- including once before the
;; first park -- so a codec that hot-added during bring-up (after the boot W1C but
;; before the watcher started) is caught immediately rather than waiting for an
;; unrelated MSI. STATESTS is sticky, so the pending bit is still there to find.
;;
;; This is the SOLE msi-wait caller on the HDA MSI handle: the wake bridge has one
;; parked-waiter slot per MSI, so a second msi-wait caller would silently steal it.
;; hda-verb! deliberately POLLS the RIRB (via wait-until) instead of waiting on the
;; MSI, specifically to keep this watcher the only waiter.
(define (start-codec-watcher regs msi drvloop)
  (spawn-restricted '()
    (lambda ()
      (let loop ((seen (msi-count msi)))
        (let ((sts (r16 regs STATESTS)))
          (if (not (= sts 0))
              (begin (w16! regs STATESTS sts)              ; W1C the state-change bits
                     (send drvloop (list 'codec-change)))))
        (msi-wait msi seen)
        (loop (msi-count msi))))))

;; --- the long-lived driver context -------------------------------------------
;; After bring-up the spawned context becomes the audio card's service loop. It
;; threads three pieces of state: `eps` = the live endpoint list (each endpoint
;; carries its own codec handle, so this can span codecs and change on hotplug),
;; `cur` = the playing output buffers (so the output stream's DMA source stays
;; live), and `cap` = the capture state (buf bdl nbytes) or #f. `rescan` is a thunk
;; that re-probes every codec and returns a fresh endpoint list -- the codec-hotplug
;; reconciliation. It answers coreaudio: (tone)/(play ...) output;
;; (set-volume)/(get-volume)/(mute) per endpoint; (capture-start ep-id)/
;; (capture-read reply)/(capture-pos reply)/(capture-stop) input; (endpoints reply)
;; and (get-status reply) introspection; (codec-change) re-enumerates after a
;; controller state-change interrupt.
(define (hda-driver-loop regs rescan eps0 refs)
  (let loop ((eps eps0) (cur refs) (cap #f))
    (let ((m (recv)))
      (cond
        ((eq? (car m) 'tone)
         (loop eps (play-tone! regs TONE-HZ TONE-AMP TONE-FRAMES) cap))
        ((eq? (car m) 'play)            ; (play freq amp frames)
         ;; Validate before synthesis: freq 0 divides by zero in fill-tone!, amp
         ;; outside [1,32767] overflows the signed-16 sample, frames 0 writes
         ;; CBL=0 (an undefined running stream). A bad request is dropped, not fatal.
         (let ((freq (nth m 1)) (amp (nth m 2)) (frames (nth m 3)))
           (if (and (> freq 0) (> amp 0) (< amp 32768) (> frames 0))
               (loop eps (play-tone! regs freq amp frames) cap)
               (begin (lg "play: bad params, ignored")
                      (loop eps cur cap)))))
        ((eq? (car m) 'endpoints)       ; (endpoints reply)
         (send (nth m 1) (endpoint-descs eps)) (loop eps cur cap))
        ((eq? (car m) 'set-volume)      ; (set-volume ep-id vol)
         (let ((ep (ep-by-id eps (nth m 1))))
           (if ep (set-endpoint-volume! ep (nth m 2))
               (begin (lg "set-volume: no endpoint " (nth m 1)))))
         (loop eps cur cap))
        ((eq? (car m) 'get-volume)      ; (get-volume ep-id reply)
         (let ((ep (ep-by-id eps (nth m 1))))
           (send (nth m 2) (if ep (ep-vol ep) #f))) (loop eps cur cap))
        ((eq? (car m) 'mute)            ; (mute ep-id on?)
         (let ((ep (ep-by-id eps (nth m 1))))
           (if ep (set-endpoint-mute! ep (nth m 2)))) (loop eps cur cap))
        ((eq? (car m) 'capture-start)   ; (capture-start ep-id)
         (let ((ep (ep-by-id eps (nth m 1))))
           (if (and ep (eq? (ep-dir ep) 'in))
               (loop eps cur (capture-start! regs ep))
               (begin (lg "capture-start: no input endpoint " (nth m 1)) (loop eps cur cap)))))
        ((eq? (car m) 'capture-read)    ; (capture-read reply) -> a copy of the ring, or #f
         (send (nth m 1) (if cap (copy-bytes (car cap) 0 (caddr cap)) #f))
         (loop eps cur cap))
        ((eq? (car m) 'capture-pos)     ; (capture-pos reply) -> DMA position, or #f
         (send (nth m 1) (if cap (capture-pos regs) #f)) (loop eps cur cap))
        ((eq? (car m) 'capture-stop)    ; (capture-stop)
         (if cap (stream-stop! regs (in-sd-base regs)))
         (loop eps cur #f))
        ((eq? (car m) 'codec-change)    ; controller state-change: re-enumerate codecs
         ;; A rescan invalidates the old endpoint set, so stop any active capture
         ;; (its input endpoint may be gone) -- the client must capture-start afresh
         ;; against the new endpoints.
         (if cap (stream-stop! regs (in-sd-base regs)))
         (loop (rescan) cur #f))
        ((eq? (car m) 'jack-poll)       ; periodic: notice plug/unplug on jacks
         (poll-jacks! eps) (loop eps cur cap))
        ((eq? (car m) 'get-status)
         (send (nth m 1) 'playing) (loop eps cur cap))
        (else (loop eps cur cap))))))

;; --- bring-up body (runs in the spawned, yielding context) -------------------
;; Mirrors module_init + hdaudio_initialize, scoped to controller + codec
;; enumeration. On success it logs, sets up MSI, and registers with coreaudio.
;; FIRE-AND-FORGET: like ahci-bringup, this runs in a spawned context so its
;; reset/settle waits yield; init does not await it. Any failure logs + returns.
(define (hdaudio-bringup regs ecam audio name)
  ;; reset the controller
  (if (not (hda-reset! regs))
      (begin (lg "controller reset timeout") 'fail)
      (begin
        ;; codecs report presence on STATESTS shortly after CRST deassert; give
        ;; them a beat to settle. (~1ms covers the 521us spec wait.)
        (sleep 1000000)
        (let* ((statests (r16 regs STATESTS))
               (corb-ent (ring-entcnt (szcap-of regs CORBSIZE)))
               (rirb-ent (ring-entcnt (szcap-of regs RIRBSIZE))))
          (lg "reset OK statests=" statests " corb-ent=" corb-ent " rirb-ent=" rirb-ent)
          ;; enable codec wake/state-change reporting (wakeen=0xFFFF) so a codec
          ;; hot-add/remove raises the controller state-change interrupt.
          (w16! regs WAKEEN #xFFFF)
          ;; Always stand up the CORB/RIRB rings + MSI, EVEN with no codec present
          ;; at boot, so a codec hot-added later can be enumerated. CORB+RIRB live in
          ;; ONE 32-bit DMA buffer: CORB (corb-ent u32s) at offset 0, RIRB (rirb-ent
          ;; 8-byte entries) after it; dma-alloc-32 keeps the phys addr < 4GB so the
          ;; 32-bit lower-base alone suffices (upper = 0).
          (let* ((corb-bytes (* corb-ent 4))
                 (rirb-bytes (* rirb-ent 8))
                 (rirb-off   corb-bytes)
                 (ring (dma-alloc-32 (+ corb-bytes rirb-bytes)))
                 (ring-phys (bytes-phys ring))
                 ;; hda-verb! uses one slot index into both rings (CORB slot*4, RIRB
                 ;; slot*8), so it must wrap at the SMALLER ring's count. (QEMU: both 256.)
                 (slot-cnt (if (< corb-ent rirb-ent) corb-ent rirb-ent))
                 (st (make-cell 0)))   ; next CORB slot index (cell, mutable)
            (corb-setup! regs ring-phys corb-ent)               ; CORB at ring base
            (rirb-setup! regs (+ ring-phys rirb-off) rirb-ent)  ; RIRB after it
            ;; Clear the boot state-change bits NOW, BEFORE the scan: scan-all-codecs
            ;; finds present codecs by probing (a vendor-id verb), not via STATESTS,
            ;; so clearing here loses nothing -- and any codec that hot-adds DURING
            ;; the scan sets a fresh STATESTS bit that survives (it is sticky) until
            ;; the watcher reconciles it, so the boot window is not a blind spot.
            (w16! regs STATESTS (r16 regs STATESTS))
            (let ((msi (pci-setup-msi ecam)))
              (lg "msi=" msi)
              ;; Scan every codec, enumerate + configure all endpoints, register the
              ;; set with coreaudio. (rescan re-runs this on a codec-change.) Arm the
              ;; state-change watcher and play the bring-up tone if there is an
              ;; output -- then serve the card forever.
              (let* ((rescan (lambda ()
                               (let ((neps (scan-all-codecs regs ring rirb-off slot-cnt st)))
                                 (lg "codec-change -> " (length neps) " endpoint(s)")
                                 (log-endpoints neps)
                                 neps)))
                     (eps (scan-all-codecs regs ring rirb-off slot-cnt st)))
                (log-endpoints eps)
                (send audio (list 'register name (self) (endpoint-descs eps)))
                (lg "registered " name " with coreaudio (" (length eps) " endpoints)")
                (start-codec-watcher regs msi (self))
                (start-jack-poller (self))          ; jack plug/unplug detection
                (hda-driver-loop
                 regs rescan eps
                 (if (primary-output eps)
                     (play-tone! regs TONE-HZ TONE-AMP TONE-FRAMES)
                     '())))))))))

;; --- entry point -------------------------------------------------------------
;; hdaudio-init brings up ONE HD Audio controller: `audio` is the coreaudio service
;; handle, `name` the card name to register (e.g. 'hda0), `ecam` the controller's
;; ECAM. init enumerates controllers (pci-find-class-all) and calls this per card,
;; so a machine with two sound cards brings both up as hda0/hda1. pci-find-class is
;; still exported as find-hda for the single-card / REPL convenience path.
(define (find-hda)
  (pci-find-class HDA-CLASS HDA-SUBCLASS))

(define (hdaudio-init audio name ecam)
  (if (not ecam)
      (begin (lg "no device present") #f)
      (let ((cfg (mmio-map ecam 4096)))
        (pci-enable-mem-bus-master! cfg)
        ;; HDA register block = BAR0. If firmware never configured it (base 0),
        ;; self-assign and re-read.
        (let ((bar-phys (let ((b (bar-base cfg HDA-BAR)))
                          (if (= b 0)
                              (begin (pci-assign-bars ecam) (bar-base cfg HDA-BAR))
                              b))))
          (if (or (not bar-phys) (= bar-phys 0))
              (begin (lg "no MMIO BAR") #f)
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
                  (lambda () (hdaudio-bringup regs ecam audio name)))
                'hdaudio-spawned)))))) ) ; last ) closes (define-module hdaudio ...)
