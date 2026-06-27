;; init: the privileged system initializer -- the first Lisp the OS runs, and the
;; single place boot POLICY lives. The kernel only loads this module and calls
;; (system-init); everything about WHAT comes up and with WHICH capabilities is
;; here, not hardcoded in C. (Replaces the former setup_input_service/discover_nic
;; in modules/SysLisp.)
;;
;; Capability posture (W7). `init` is loaded at boot with full (root) authority,
;; so its (import ps2 virtio-net) succeeds: each driver module captures exactly
;; the sys-* authority it needs into its own closures at load time. The long-lived
;; service contexts init then spawns are RESTRICTED -- (spawn-restricted '() ...)
;; grants them NO further import authority -- so a wedged or compromised driver
;; loop cannot (import sys-pci) to reach new hardware. They still work because the
;; module boundary already handed them their primitives lexically; the empty grant
;; only forbids acquiring MORE. This is the knob a future serial REPL rides: root
;; when debugging the OS, a narrow grant when not.

(define-module init
  (export system-init start-repl play-tone set-vol)
  (import coreinput coreaudio corepower corestorage coredisplay corecompositor
          corenetwork
          corenetdebug coreusb ps2 virtio-net rtl8139 rtl8169 virtio-gpu lfb ahci
          cardfs hdaudio uhci xhci ehci usb-hid usb-hub usb-storage usb-audio
          graphics font ttf sys-pci sys-cmdline sys-initrd sys-mmio sys-reg
          sys-shm-mint)

  ;; Parse a dotted-quad "A.B.C.D" into (A B C D), or #f if malformed. Used for
  ;; the cardinal.ip= static-address override (digits/dots only, exactly 4 octets,
  ;; each 0..255). A bad value returns #f and falls back to DHCP rather than
  ;; silently configuring an out-of-range address that nothing would ever match.
  (define (parse-ipv4 s)
    (let loop ((chars (string->list s)) (cur 0) (have #f) (acc '()))
      (if (null? chars)
          (let ((acc2 (if have (cons cur acc) acc)))
            (if (= (length acc2) 4) (reverse acc2) #f))
          (let ((c (char->integer (car chars))))
            (cond
              ((= c 46)                          ; '.' -> next octet
               (if have (loop (cdr chars) 0 #f (cons cur acc)) #f))
              ((and (>= c 48) (<= c 57))         ; '0'..'9'
               (let ((n (+ (* cur 10) (- c 48))))
                 (if (> n 255) #f (loop (cdr chars) n #t acc))))
              (else #f))))))

  ;; The static address pinned on the kernel command line, or #f to use DHCP.
  (define (static-ip)
    (let ((v (cmdline-get "cardinal.ip=")))
      (if v (parse-ipv4 v) #f)))

  ;; Bring up keyboard input: start the generic input service (coreinput, a
  ;; reusable server module -- mechanism), run i8042 bring-up here in the root
  ;; init context (ps2-init wants port-I/O authority, which init has and the
  ;; restricted pump does not need to repeat), then spawn the keyboard pump as a
  ;; restricted context that feeds the service. This wiring -- WHICH driver backs
  ;; the input service -- is policy, so it lives in init, not in coreinput.
  (define (setup-input)
    (let ((input (start-input-service)))   ; the async device-table server
      (ps2-init)                           ; i8042 controller + keyboard bring-up
      (spawn-restricted '()                ; the keyboard pump needs no import authority
        (lambda () (ps2-keyboard-driver input)))
      input))

  ;; The running coreaudio service handle, published by system-init so the serial
  ;; REPL can drive playback interactively (start-repl imports `play-tone` into the
  ;; REPL env). #f until the audio service is up.
  (define audio-service #f)

  ;; Cross-core rendezvous for the compositor owner handle. The owner (core 0) comes
  ;; up async inside a spawned context, but per-core SHARD instances (born on APs by
  ;; the set-per-core-init hook) need its handle to register their grant-shared layer.
  ;; A spawned context CANNOT `set!` a module global (only the root eval can), so the
  ;; handoff is by message-passing, not a shared binding: a small rendezvous context
  ;; holds the owner and answers `(get-owner reply)`, QUEUING the request until the
  ;; bring-up sends `(set-owner comp)`. The rendezvous handle itself is set! by the
  ;; ROOT (system-init) BEFORE the APs go live, so reading it on an AP is safe (a
  ;; read of a module global works from any context; only set! is root-only) and the
  ;; per-core barrier publishes it. #f until system-init creates it.
  (define compositor-rendezvous #f)

  ;; Spawn the owner rendezvous (see compositor-rendezvous). It parks holding the
  ;; owner once known, replying to every queued/late `get-owner` -- so a shard that
  ;; asks before the owner is up simply blocks on recv until it arrives.
  (define (make-owner-rendezvous)
    (spawn-restricted '()
      (lambda ()
        (let loop ((owner #f) (waiters '()))
          (let ((m (recv)))
            (cond ;; first valid set-owner wins (idempotent bring-up); the owner must
                  ;; be a context, else a stray (set-owner #f) would poison every shard.
                  ((and (not owner) (pair? m) (eq? (car m) 'set-owner) (ctx? (cadr m)))
                   (for-each (lambda (w) (send w (list 'owner (cadr m)))) waiters)
                   (loop (cadr m) '()))
                  ((and (pair? m) (eq? (car m) 'get-owner) (ctx? (cadr m)))
                   (if owner
                       (begin (send (cadr m) (list 'owner owner)) (loop owner waiters))
                       (loop owner (cons (cadr m) waiters))))
                  (else (loop owner waiters))))))))

  ;; REPL command: play a tone through the audio stack. Routes through the live
  ;; coreaudio service to the registered card 'hda0 -- the exact path a real client
  ;; uses -- so it doubles as an end-to-end check of the hdaudio playback path.
  ;;   (play-tone)               default bring-up tone (500 Hz)
  ;;   (play-tone freq)          freq Hz at the default amplitude / length
  ;;   (play-tone freq amp)      + amplitude (1..32767)
  ;;   (play-tone freq amp n)    + length in frames (each frame = 1/48000 s)
  ;; Returns 'playing, or 'no-audio if no HD Audio controller came up.
  (define (play-tone . args)
    (cond
      ((not audio-service) 'no-audio)
      ((null? args)
       (send audio-service (list 'tone 'hda0)) 'playing)
      (else
       (let ((freq   (car args))
             (amp    (if (>= (length args) 2) (cadr args) 8000))
             (frames (if (>= (length args) 3) (caddr args) 4800)))
         ;; Guard arg types here: a non-integer would otherwise reach the card
         ;; context's (> freq 0) and kill it (the card handle then goes dead until
         ;; reboot). The card ALSO range-checks; this just stops bad types early.
         (if (and (integer? freq) (integer? amp) (integer? frames))
             (begin (send audio-service (list 'play 'hda0 freq amp frames)) 'playing)
             'bad-args)))))

  ;; REPL command: set an endpoint's volume on card 'hda0, through coreaudio.
  ;;   (set-vol ep-id vol)   vol 0..100 (0 mutes); ep-id from the boot endpoint log
  ;; Returns 'ok, 'no-audio, or 'bad-args.
  (define (set-vol ep-id vol)
    (cond
      ((not audio-service) 'no-audio)
      ((and (integer? ep-id) (integer? vol))
       (send audio-service (list 'set-volume 'hda0 ep-id vol)) 'ok)
      (else 'bad-args)))

  ;; Capture self-check (gated on cardinal.mictest): in its own context, find card
  ;; hda0's first input endpoint, start capturing, sample the DMA position twice,
  ;; and log whether it advanced (the capture engine is live) plus the captured
  ;; byte count. QEMU has no mic source so the PCM is silence, but the input stream
  ;; DMA still runs -- LPIB advancing is the proof the path works end to end.
  (define (start-mictest)
    (spawn-restricted '()
      (lambda ()
        (sleep 2000000000)                                  ; let hda0 finish bring-up
        (send audio-service (list 'endpoints 'hda0 (self)))
        (let find ((es (recv)))
          (cond
            ((null? es) (display "[mictest] no input endpoint") (newline))
            ((eq? (cadr (car es)) 'in)
             (let ((id (car (car es))))
               (send audio-service (list 'capture-start 'hda0 id))
               (sleep 300000000)
               (send audio-service (list 'capture-pos 'hda0 (self)))
               (let ((p1 (recv)))
                 (sleep 100000000)
                 (send audio-service (list 'capture-pos 'hda0 (self)))
                 (let ((p2 (recv)))
                   (send audio-service (list 'capture-read 'hda0 (self)))
                   (let ((buf (recv)))
                     (send audio-service (list 'capture-stop 'hda0))
                     (display "[mictest] ep ") (display id)
                     (display " pos ") (display p1) (display " -> ") (display p2)
                     (display (if (and p1 p2 (not (= p1 p2))) " ADVANCING" " (no advance)"))
                     (display " captured-bytes=") (display (if buf (bytes-length buf) 0))
                     (newline))))))
            (else (find (cdr es))))))))

  ;; --- graphics demo (gated on cardinal.gfxdemo) ------------------------------
  ;; Draws a UI demo frame with the graphics + font libraries -- the end-to-end proof
  ;; of the 2D API over a real framebuffer. Off by default; the gfxdemo-image ISO
  ;; boots with the flag (capture via run-qemu.sh SCREENSHOT=).

  ;; An off-screen ARGB image surface (for the alpha-blit demo).
  (define (gfx-image w h) (make-surface (make-bytes (* w h 4)) w h (* w 4)))

  (define (draw-demo-ui surf fnt tf)
    (let ((W (surface-width surf)) (H (surface-height surf))
          (white (rgb surf 240 240 245)))
      (clear surf (rgb surf 28 30 44))                 ; desktop background
      ;; top bar + title (antialiased TrueType when available, else the bitmap font)
      (fill-rect surf 0 0 W 46 (rgb surf 46 50 72))
      (if tf (ttf-draw-text surf tf 16 9 "Cardinal  -  TrueType text rendering" white 26)
          (if fnt (draw-text surf fnt 14 7 "Cardinal;  graphics demo" white 0 #f 2)))
      ;; a "window" with a title bar + a TrueType showcase + the bitmap font
      (let ((wx 64) (wy 92) (ww 540) (wh 372) (ink (rgb surf 38 40 52)))
        (fill-rect surf wx wy ww wh (rgb surf 244 245 250))
        (draw-rect surf wx wy ww wh 2 (rgb surf 78 90 132))
        (fill-rect surf (+ wx 2) (+ wy 2) (- ww 4) 30 (rgb surf 58 120 200))
        (if tf (ttf-draw-text surf tf (+ wx 10) (+ wy 7) "Hello, Cardinal" white 18))
        ;; antialiased TrueType at several point sizes (one stb_truetype font, cached)
        (if tf
            (begin
              (ttf-draw-text surf tf (+ wx 14) (+ wy 40)
                             "Antialiased TrueType via stb_truetype" ink 22)
              (ttf-draw-text surf tf (+ wx 14) (+ wy 74)
                             "The quick brown fox jumps over 12,345 dogs." ink 16)
              (ttf-draw-text surf tf (+ wx 14) (+ wy 100) "Sizes:" (rgb surf 120 120 140) 14)
              (ttf-draw-text surf tf (+ wx 90) (+ wy 96) "14" ink 14)
              (ttf-draw-text surf tf (+ wx 130) (+ wy 92) "20" ink 20)
              (ttf-draw-text surf tf (+ wx 185) (+ wy 86) "28" ink 28)
              (ttf-draw-text surf tf (+ wx 255) (+ wy 76) "40" (rgb surf 180 70 70) 40)))
        ;; the bitmap font, for comparison
        (if fnt
            (begin
              (draw-text surf fnt (+ wx 14) (+ wy 168)
                         "Bitmap font (gfx-glyph!), 1x / 2x:" ink 0 #f 1)
              (draw-text surf fnt (+ wx 14) (+ wy 188)
                         "ABCDEFG abcdefg 0123456789" ink 0 #f 1)
              (draw-text surf fnt (+ wx 14) (+ wy 210) "scaled" (rgb surf 70 130 70) 0 #f 2))))
      ;; shapes panel on the right
      (let ((px 650) (py 110))
        (fill-circle surf (+ px 64) (+ py 64) 54 (rgb surf 232 124 80))
        (draw-circle surf (+ px 64) (+ py 64) 54 white)
        (fill-rect surf px (+ py 150) 128 76 (rgb surf 82 200 142))
        (draw-rect surf px (+ py 150) 128 76 3 white)
        (let loop ((i 0))                              ; a fan of lines
          (if (< i 11)
              (begin (draw-line surf px (+ py 260) (+ px (* i 13)) (+ py 344)
                                (rgb surf 120 180 240))
                     (loop (+ i 1)))))
        (let ((sw (list (rgb surf 230 60 60) (rgb surf 230 140 40) (rgb surf 230 215 50)
                        (rgb surf 70 200 90) (rgb surf 60 160 230) (rgb surf 96 96 224)
                        (rgb surf 170 80 200))))
          (let loop ((i 0))                            ; rainbow swatches
            (if (< i 7)
                (begin (fill-rect surf (+ px (* i 26)) (+ py 360) 24 24 (nth sw i))
                       (loop (+ i 1)))))))
      ;; a translucent panel over the desktop, showing alpha compositing
      (let ((img (gfx-image 460 92)))
        (clear img (argb img 165 54 92 168))           ; ~65% opaque blue
        (if tf
            (begin
              (ttf-draw-text img tf 14 10 "Alpha-blended panel (blit-alpha)" white 20)
              (ttf-draw-text img tf 14 48 "antialiased text over a translucent surface" white 15))
            (if fnt (draw-text img fnt 14 12 "alpha-blended panel (blit-alpha)" white 0 #f 1)))
        (blit-alpha surf img 64 486))
      'done))

  ;; Map the boot framebuffer into a live graphics surface. A framebuffer is shared
  ;; memory, so it must be mmio-mapped IN the drawing context -- a get-framebuffer
  ;; message would deliver a COPY (copy-on-send), whose writes never reach the
  ;; scanout. init holds the sys-mmio/sys-reg authority to map it (the same geometry
  ;; lfb reads). Returns a 0xRRGGBB surface, or #f if there is no boot framebuffer.
  (define GFX-FB-PATH "HW/BOOTINFO/FRAMEBUFFER")
  (define (reg-or path key d) (let ((v (reg-read-uint path key))) (if v v d)))
  (define (gfx-map-framebuffer)
    (let ((phys   (reg-read-uint GFX-FB-PATH "PHYS_ADDR"))
          (pitch  (reg-read-uint GFX-FB-PATH "PITCH"))
          (width  (reg-read-uint GFX-FB-PATH "WIDTH"))
          (height (reg-read-uint GFX-FB-PATH "HEIGHT")))
      (if (or (not phys) (not pitch) (not width) (not height)
              (= phys 0) (= pitch 0) (= width 0) (= height 0))
          #f
          ;; WC-map the scanout: the demo composes in a cached back-buffer and
          ;; streams finished frames here, so write-combining (the CPU coalesces
          ;; sequential stores into bursts) is exactly the right mapping for a
          ;; write-only framebuffer -- many times the throughput of a UC mapping.
          (make-surface* (mmio-map-wc phys (* pitch height)) width height pitch
                         (reg-or GFX-FB-PATH "RED_OFFSET" 16)
                         (reg-or GFX-FB-PATH "GREEN_OFFSET" 8)
                         (reg-or GFX-FB-PATH "BLUE_OFFSET" 0) '()))))

  ;; Micro-benchmark the WC + double-buffer pipeline. bytes/ns == GB/s, so *1000
  ;; gives MB/s. CAVEAT: under QEMU/KVM the scanout is EMULATED VRAM behind the
  ;; display's dirty-page tracking (KVM write-protects framebuffer pages and faults
  ;; to mark them dirty for the refresh), a well-known slow path where -- as the
  ;; community puts it -- "write-combining doesn't do much in QEMU". So the absolute
  ;; flush numbers reflect the emulator, NOT real hardware, where WC takes a
  ;; framebuffer from a few hundred MB/s to >1 GB/s (Linux vesafb/efifb map it WC
  ;; for exactly this reason; ~6x in published vesa-MTRR measurements). The bench
  ;; flushes the SAME frame through both a WC and a UC mapping to show they tie
  ;; here; the cached back-buffer compose is real RAM and IS representative.
  (define (mb-per-s bytes ns) (if (= ns 0) 0 (quotient (* bytes 1000) ns)))
  (define (time-copies dst src iters)              ; ns for `iters` full-buffer copies
    (let ((len (bytes-length src)) (t0 (uptime-ns)))
      (let loop ((i 0)) (if (< i iters) (begin (bytes-copy! dst 0 src 0 len) (loop (+ i 1)))))
      (- (uptime-ns) t0)))
  (define (gfx-benchmark db fnt tf)
    (let* ((back (db-back db)) (front (db-front db))
           (W (surface-width back)) (H (surface-height back))
           (fb (surface-fb back)) (wcfb (surface-fb front))
           (fbytes (bytes-length fb))
           (ucfb (mmio-map (bytes-phys wcfb) fbytes)) ; a UC view of the same scanout
           (bg (rgb back 28 30 44)))
      ;; 0) DIAGNOSTIC: stream stores to the SAME physical scratch RAM through
      ;; WB / WC / UC mappings. On real hardware WB > WC >> UC. Observed here:
      ;; WB is fast but WC == UC (both slow) -- and UC being slow proves guest PAT
      ;; IS honored (modern Intel w/ self-snoop; KVM does NOT force WB), so this is
      ;; NOT "the hypervisor ignores PAT". WC simply yields no write-combine speedup
      ;; under EPT here, the same reason the WC framebuffer flush ties UC below. The
      ;; WC win shows up on bare metal, not under this emulator.
      (let* ((sbytes (* 1024 1024))
             (sUC (dma-alloc sbytes))                  ; dma-alloc returns a UC mapping
             (sWC (mmio-map-wc (bytes-phys sUC) sbytes))
             (sWB (make-bytes sbytes))
             (words (quotient sbytes 4)))
        (define (fill-bw b)
          (let ((t0 (uptime-ns)))
            (let loop ((i 0)) (if (< i 30) (begin (bytes-fill32! b 0 words #x223344) (loop (+ i 1)))))
            (mb-per-s (* sbytes 30) (- (uptime-ns) t0))))
        (display "[gfx-bench] 1MB scratch store WB: ") (display (fill-bw sWB))
        (display " | WC: ") (display (fill-bw sWC))
        (display " | UC: ") (display (fill-bw sUC)) (display " MB/s") (newline))
      ;; 1) cached back-buffer compose -- real RAM, representative of any machine.
      (let ((t0 (uptime-ns)))
        (let loop ((i 0)) (if (< i 20) (begin (clear back bg) (loop (+ i 1)))))
        (let ((dt (- (uptime-ns) t0)))
          (display "[gfx-bench] back-buffer compose (cached/WB): ")
          (display (mb-per-s (* fbytes 20) dt)) (display " MB/s") (newline)))
      ;; 2) flush bandwidth: WC vs UC mapping of the SAME scanout, identical copy.
      (let ((wc (time-copies wcfb fb 5)) (uc (time-copies ucfb fb 5)))
        (display "[gfx-bench] flush WC: ") (display (mb-per-s (* fbytes 5) wc))
        (display " MB/s | UC: ") (display (mb-per-s (* fbytes 5) uc))
        (display " MB/s (tie under QEMU dirty-tracking; real HW WC ~6x UC)") (newline))
      ;; 3) full UI frame: compose + WC flush. Report frame time + fps (x100 for
      ;; one decimal, since a trapped-VRAM frame is ~1 fps under the emulator).
      (let ((t0 (uptime-ns)))
        (let loop ((i 0))
          (if (< i 3) (begin (draw-demo-ui back fnt tf) (db-flush db) (loop (+ i 1)))))
        (let* ((dt (- (uptime-ns) t0))
               (cfps (if (= dt 0) 0 (quotient (* 300000000000 1) dt))))
          (display "[gfx-bench] full UI frame (compose+WC flush): ")
          (display (quotient dt (* 3 1000))) (display " us, ")
          (display (quotient cfps 100)) (display ".") (display (modulo cfps 100))
          (display " fps @ ") (display W) (display "x") (display H) (newline)))))

  ;; Benchmark rendering through the virtio-gpu driver -- the MODERN path that
  ;; sidesteps the trapped-framebuffer problem entirely. The driver's scanout
  ;; backing is guest RAM (allocated WRITE-BACK via dma-alloc-wb); the device reads
  ;; it coherently, so we map the SAME physical pages WB (mmio-map-wb) and compose
  ;; into that fast cached view. A virtqueue `(flush)` (TRANSFER_TO_HOST_2D +
  ;; RESOURCE_FLUSH) then pushes the frame device-side -- no MMIO framebuffer
  ;; writes, no per-page dirty-tracking fault.
  ;;
  ;; gpu-frame! is one synchronous frame: fence the composed stores, then send a
  ;; flush WITH a reply target and block on the ack -- the driver acks only after
  ;; the flush's controlq round-trip completes, so this both pushes the frame and
  ;; times it (a fire-and-forget (flush) without the reply would not let us pace).
  (define (gpu-frame! gpu)              ; one fenced, synchronous flush (waits for ack)
    (sfence)
    (send gpu (list 'flush (self)))
    (recv))
  (define (gpu-benchmark gpu fnt tf)
    (send gpu (list 'get-framebuffer (self)))
    (let ((r (recv)))                   ; (w h phys) for scanout 0, or #f
      (if (not (pair? r))
          (begin (display "[gpu-bench] virtio-gpu returned no scanout") (newline))
          (let* ((w (car r)) (h (cadr r)) (phys (caddr r))
                 (stride (* w 4)) (fbytes (* stride h))
                 ;; the reply's fb bytes are a copy-on-send shadow (phys lost); map
                 ;; the backing PHYS the driver sent us, WRITE-BACK, and compose there.
                 (wb   (mmio-map-wb phys fbytes))
                 ;; virtio-gpu's VIRTIO_GPU_FORMAT_X8R8G8B8 is MEMORY byte order
                 ;; [X,R,G,B] (byte0=X), so channel bit-offsets are R=8/G=16/B=24 --
                 ;; NOT the std-vga 0xRRGGBB (16/8/0). Verified by pixel sampling.
                 (surf (make-surface* wb w h stride 8 16 24 '()))
                 (bg   (rgb surf 28 30 44)))
            (display "[gpu-bench] virtio-gpu scanout ") (display w) (display "x") (display h)
            (display " backing WB-mapped") (newline)
            ;; 1) compose into the WB backing (real cached RAM -- representative).
            (let ((t0 (uptime-ns)))
              (let loop ((i 0)) (if (< i 20) (begin (clear surf bg) (loop (+ i 1)))))
              (let ((dt (- (uptime-ns) t0)))
                (display "[gpu-bench] backing compose (WB): ")
                (display (mb-per-s (* fbytes 20) dt)) (display " MB/s") (newline)))
            ;; 2) synchronous flush round-trip, no compose: the per-frame device +
            ;; IPC cost (whole-frame transfer-2d + resource-flush + the ack barrier).
            (let ((t0 (uptime-ns)))
              (let loop ((i 0)) (if (< i 20) (begin (gpu-frame! gpu) (loop (+ i 1)))))
              (let* ((dt (- (uptime-ns) t0)))
                (display "[gpu-bench] flush round-trip (no compose): ")
                (display (quotient dt (* 20 1000))) (display " us/frame, ")
                (display (mb-per-s (* fbytes 20) dt)) (display " MB/s") (newline)))
            ;; 3) full UI frame: compose + flush -> the real achievable fps.
            (let ((t0 (uptime-ns)))
              (let loop ((i 0))
                (if (< i 10) (begin (draw-demo-ui surf fnt tf) (gpu-frame! gpu) (loop (+ i 1)))))
              (let* ((dt (- (uptime-ns) t0))
                     (cfps (if (= dt 0) 0 (quotient (* 100000000000 10) dt))))
                (display "[gpu-bench] full UI frame (compose+flush): ")
                (display (quotient dt (* 10 1000))) (display " us, ")
                (display (quotient cfps 100)) (display ".") (display (modulo cfps 100))
                (display " fps @ ") (display w) (display "x") (display h) (newline)))
            ;; 4) dirty-rect: the SAME frame, but only the moving box is recomposed
            ;; and flushed each step -- the win when a UI changes a small region.
            (gpu-dirty-bench gpu surf fnt tf w h)
            surf))))

  ;; Animate a box over the desktop background, recomposing + flushing only its
  ;; damage each frame (erase old rect to bg, draw new rect, flush both). Contrast
  ;; with step 3: there the whole UI is recomposed; here the per-frame work is a
  ;; ~140x140 region, so the rate is bounded by the flush round-trip, not compose.
  ;; The box stays over the uniform desktop, so erase-to-bg restores correctly
  ;; (no underlying UI to repaint -- that is the compositor's job, a later step).
  (define (gpu-dirty-bench gpu surf fnt tf w h)
    (let ((bg  (rgb surf 28 30 44)) (ink (rgb surf 240 240 245))
          (box (rgb surf 232 124 80)) (bw 140) (bh 140) (by 300) (step 8))
      (clear surf bg)
      (if tf (ttf-draw-text surf tf 40 40
                            "Dirty-rect: only the moving box is recomposed + flushed" ink 22))
      (sfence) (send gpu (list 'flush (self))) (recv)   ; push the static scene once
      (let ((dmg (make-damage)) (frames 200) (t0 (uptime-ns)))
        (let loop ((i 0) (x 40) (px 40))
          (if (< i frames)
              (begin
                (fill-rect surf px by bw bh bg)           ; erase previous box
                (fill-rect surf x  by bw bh box)          ; draw at new position
                (damage-add! dmg px by bw bh)
                (damage-add! dmg x  by bw bh)
                (sfence)
                (send gpu (list 'flush-rects (damage-rects dmg) (self))) (recv)
                (damage-clear! dmg)
                (let ((nx (+ x step)))
                  (loop (+ i 1) (if (> (+ nx bw) w) 40 nx) x)))
              (let* ((dt (- (uptime-ns) t0))
                     (cfps (if (= dt 0) 0 (quotient (* 100000000000 frames) dt))))
                (display "[gpu-bench] dirty-rect anim (")
                (display bw) (display "x") (display bh) (display " box): ")
                (display (quotient dt (* frames 1000))) (display " us/frame, ")
                (display (quotient cfps 100)) (display ".") (display (modulo cfps 100))
                (display " fps") (newline)))))))

  ;; The virtio-gpu demo: compose into the WB-mapped scanout backing, benchmark the
  ;; render path, then redraw a stable frame. The caller gates this on a virtio-gpu
  ;; PCI device being present; we then sleep to let bring-up finish before the
  ;; (get-framebuffer) handshake. NOTE: if the device is present but bring-up fails
  ;; to enable a scanout (the driver context then exits), the get-framebuffer recv
  ;; blocks forever -- acceptable for this demo (QEMU always enables a scanout); a
  ;; production consumer would discover the GPU via the display service's
  ;; registration (which only fires on success) rather than a bare handle + recv.
  (define (start-gpu-demo gpu)
    (let ((fontbytes (initrd-file FONT8X16-PATH))
          (ttfbytes (initrd-file TTF-FONT-PATH)))
      (spawn-restricted '()
        (lambda ()
          (sleep 800000000)             ; let virtio-gpu finish async bring-up
          (let ((fnt (if fontbytes (make-font fontbytes FONT8X16-W FONT8X16-H) #f))
                (tf  (if ttfbytes (make-ttf-font ttfbytes) #f)))
            (let ((surf (gpu-benchmark gpu fnt tf)))
              (if surf
                  (let loop ((k 0))
                    (if (< k 6)
                        (begin (draw-demo-ui surf fnt tf) (gpu-frame! gpu)
                               (sleep 2000000000) (loop (+ k 1)))
                        'done)))))))))

  ;; Load the default font (init holds sys-initrd), map the live framebuffer, then
  ;; spawn a context that draws the demo. The surface is captured by the spawned
  ;; lambda (spawn shares captured state, unlike a message), so its draws land on
  ;; the real scanout. It repaints a few times to cover straggler kernel-debug text
  ;; (the SysDebug console shares this framebuffer), finishing silently so the final
  ;; frame is stable for a screenshot.
  (define (start-gfx-demo)
    (let ((front (gfx-map-framebuffer))
          (fontbytes (initrd-file FONT8X16-PATH))
          (ttfbytes (initrd-file TTF-FONT-PATH)))
      (if (not front)
          (begin (display "[gfx-demo] no framebuffer") (newline))
          (begin
            (display "[gfx-demo] framebuffer ") (display (surface-width front)) (display "x")
            (display (surface-height front)) (display " pitch ") (display (surface-stride front))
            (display " (WC-mapped, double-buffered)")
            (display " bitmap-font=") (display (if fontbytes (bytes-length fontbytes) 0))
            (display " ttf=") (display (if ttfbytes (bytes-length ttfbytes) 0)) (newline)
            (spawn-restricted '()
              (lambda ()
                ;; the cached back-buffer (make-bytes) must live in THIS context's
                ;; heap; the WC front is captured by identity (spawn shares state).
                (let ((db  (make-double-buffer front))
                      (fnt (if fontbytes (make-font fontbytes FONT8X16-W FONT8X16-H) #f))
                      (tf  (if ttfbytes (make-ttf-font ttfbytes) #f)))
                  (let ((back (db-back db)))
                    (draw-demo-ui back fnt tf)
                    (db-flush db)
                    ;; self-check: read a pixel back from the cached back-buffer.
                    (display "[gfx-demo] frame drawn; ")
                    (display (if (= (get-pixel back 2 220) (rgb back 28 30 44))
                                 "pixel-check OK" "pixel-check MISMATCH"))
                    (newline)
                    (gfx-benchmark db fnt tf)
                    ;; redraw a stable frame for the screenshot, then repaint a few
                    ;; times to cover straggler kernel-debug text on the scanout.
                    (draw-demo-ui back fnt tf) (db-flush db)
                    (let loop ((k 0))
                      (if (< k 6)
                          (begin (sleep 2000000000) (draw-demo-ui back fnt tf) (db-flush db)
                                 (loop (+ k 1)))
                          'done))))))))))

  ;; --- phase 4: the compositor owns the real scanout --------------------------
  ;; Build the compositor's (screen . present) target for the live display. The
  ;; compositor composites into `screen` (a cached WB back-buffer) and, after each
  ;; change, calls `present` with the damaged (x y w h) rects to push them to the
  ;; actual display. init holds the sys-mmio authority to map the scanout; the
  ;; compositor never does -- the seam is exactly one injected closure.
  ;;
  ;; virtio-gpu: ask the driver for its scanout (w h phys), map the backing WB (the
  ;; coherent cached view the device reads -- the modern path), and present by
  ;; flushing the dirty rects over the controlq. get-framebuffer BLOCKS until the
  ;; driver's async bring-up has a scanout, so this MUST run in a yielding context.
  (define (compositor-gpu-target gpu)
    (send gpu (list 'get-framebuffer (self)))
    (let ((r (recv)))
      (if (not (pair? r))
          #f
          (let* ((w (car r)) (h (cadr r)) (phys (caddr r)) (stride (* w 4))
                 ;; virtio-gpu X8R8G8B8 -> channel offsets R=8/G=16/B=24 (see gpu-bench).
                 (screen (make-surface* (mmio-map-wb phys (* stride h)) w h stride 8 16 24 '())))
            (cons screen
                  (lambda (rects)
                    (sfence)
                    (send gpu (list 'flush-rects rects (self)))
                    (recv))))))) ; block on the flush ack so frames pace to the device

  ;; Boot framebuffer (lfb / -vga std): the scanout is WC-mapped MMIO with no driver
  ;; flush. Compose into a cached WB back-buffer (make-double-buffer) and present by
  ;; copying the dirty rects back->front (db-flush-rect) after an sfence.
  (define (compositor-fb-target)
    (let ((front (gfx-map-framebuffer)))
      (if (not front)
          #f
          (let ((db (make-double-buffer front)))
            (cons (db-back db)
                  (lambda (rects)
                    (sfence)
                    (for-each (lambda (r)
                                (db-flush-rect db (nth r 0) (nth r 1) (nth r 2) (nth r 3)))
                              rects)))))))

  ;; A per-core compositor SHARD. Born on every AP by the set-per-core-init hook
  ;; (always on -- one compositor instance per core is the production model; the
  ;; cross-shard tests just consume the shards they already get). It is a FULL
  ;; compositor instance (start-compositor-service
  ;; in shard role) that hosts the opaque clients the owner routes to it -- so those
  ;; clients' windows composite on THIS core, into a grant-shared (colour,z) LAYER the
  ;; owner maps and folds into its scanout. Bring-up: rendezvous for the owner handle,
  ;; ask the scanout geometry, dma-alloc-wb a matching layer, mint rw grants, start the
  ;; shard-role service over that layer (z-base 0 -- all windows draw their z from the
  ;; owner's global counter via alloc-z), then register its handle + grants with the
  ;; owner. It holds no caps --
  ;; dma-alloc-wb / grant-mint are captured prims that work in any context.
  (define (start-compositor-shard id)
    (send compositor-rendezvous (list 'get-owner (self)))
    (let ((om (recv)))
      (if (not (and (pair? om) (eq? (car om) 'owner) (ctx? (cadr om))))
          (begin (display "[compositorshards] core ") (display id)
                 (display " FAIL no owner from rendezvous") (newline))
          (let ((owner (cadr om)))
            (send owner (list 'shard-geom (self)))
            (let ((g (recv)))                       ; (geom w h stride fmt)
              (if (not (and (pair? g) (eq? (car g) 'geom)))
                  (begin (display "[compositorshards] core ") (display id)
                         (display " FAIL no geom") (newline))
                  (let* ((w (nth g 1)) (hh (nth g 2)) (stride (nth g 3)) (fmt (nth g 4))
                         (ro (car fmt)) (go (cadr fmt)) (bo (caddr fmt))
                         (plane (* stride hh))
                         (lcb (dma-alloc-wb plane)) (lzb (dma-alloc-wb plane))
                         (gc (grant-mint lcb 'rw)) (gz (grant-mint lzb 'rw))
                         (lc (make-surface* lcb w hh stride ro go bo '()))
                         (lz (make-surface lzb w hh stride))
                         (cfg (make-shard-cfg owner lc lz 0))   ; z from the owner's global counter (alloc-z)
                         ;; shard caps: alloc/mint/revoke for client surface backings;
                         ;; no present (the owner flushes), no map (the owner maps). The
                         ;; last cap is the SHARD-MESH KEY (the rendezvous ctx -- shared
                         ;; with the owner, never a client), stamped on this shard's
                         ;; layer-update/alloc-z and checked on the owner's move-window/z.
                         (caps (make-compositor-caps dma-alloc-wb grant-mint grant-revoke #f #f
                                                     compositor-rendezvous))
                         (shard (start-compositor-service lc caps cfg)))
                    (send owner (list 'register-shard compositor-rendezvous shard gc gz))
                    (display "[compositorshards] core ") (display id)
                    (display " shard instance up + registered") (newline))))))))

  ;; Wait until the compositor owner has at least one shard registered (so an opaque
  ;; client connecting after this is deterministically ROUTED to a shard), or a
  ;; bounded deadline passes (no APs / SMP=1 -> fall through to owner-hosted). The
  ;; cross-shard input tests call this before connecting so they actually exercise
  ;; the cross-shard path when shards exist, instead of racing the per-core bring-up.
  (define (wait-for-shards comp)
    (let loop ((tries 0))
      (send comp (list 'shard-count (self)))
      (let ((n (recv)))
        (cond ((and (integer? n) (> n 0)) n)
              ((>= tries 80) 0)                ; ~8s: single-core / no shards
              (else (sleep 100000000) (loop (+ tries 1)))))))

  ;; A parameterised compositor demo client (phase 5). Each call spawns an
  ;; INDEPENDENT '(sys-shm) client context that connects on its own, creates one
  ;; surface, draws a distinct titled window into its granted backing (using the
  ;; screen pixel format the compositor advertised at connect, since gfx-blit! is a
  ;; raw copy), places it on the desktop and commits -- so a real window appears on
  ;; the real display via the compositor's flush path. Restricted to '(sys-shm): it
  ;; can map only granted memory (the window-client posture). `body`/`bar`/`rect`/
  ;; `circ` are (r g b) accent colours; `delay-ns` staggers a client's connect so a
  ;; later window lands at a higher z and the cross-client occlusion is deterministic
  ;; for the screenshot. Two of these (below) prove multi-client compositing: two
  ;; isolated contexts, two per-client handlers, one painter's pass over both.
  (define (start-compositor-demo-client comp delay-ns x y w h title body bar rect circ)
    (let ((ttfbytes (initrd-file TTF-FONT-PATH)))
      (spawn-restricted '(sys-shm)
        (lambda ()
          (import sys-shm)
          (if (> delay-ns 0) (sleep delay-ns))
          (let ((tf (if ttfbytes (make-ttf-font ttfbytes) #f)))
            (send comp (list 'connect #f (self)))
            (let ((r (recv)))                            ; (connected handler fmt)
              (if (not (eq? (car r) 'connected))
                  (begin (display "[compositor-demo] FAIL no connected reply") (newline))
                  (let* ((handler (cadr r)) (fmt (caddr r))
                         (ro (car fmt)) (go (cadr fmt)) (bo (caddr fmt)))
                    (send handler (list 'create-surface w h))
                    (let ((s (recv)))                    ; (surface id g0 g1 stride)
                      (let* ((id (cadr s)) (g0 (caddr s)) (stride (nth s 4))
                             (surf (make-surface* (map-grant g0) w h stride ro go bo '()))
                             (col (lambda (c) (rgb surf (car c) (cadr c) (caddr c)))))
                        (clear surf (col body))                          ; window body
                        (fill-rect surf 0 0 w 28 (col bar))              ; title bar
                        (if tf (ttf-draw-text surf tf 10 5 title
                                              (rgb surf 255 255 255) 18))
                        (draw-rect surf 0 0 w h 1 (rgb surf 30 40 60))   ; border
                        (fill-rect surf 28 70 110 90 (col rect))
                        (fill-circle surf (- w 110) 120 50 (col circ))
                        (send handler (list 'configure id x y #t))
                        (send handler (list 'commit id 0 '()))
                        (display "[compositor-demo] window '") (display title)
                        (display "' presented") (newline)))))))))))

  ;; The system entry point: called once on the BSP after the scheduler is live.
  ;; Each Core* service is a long-lived context; bring up the ones that exist
  ;; today (input, audio, power) and the NIC. Audio/power have no drivers feeding
  ;; them yet -- they idle parked on recv -- but the endpoints are present for the
  ;; drivers that will attach (the same posture the C servers held).
  (define (system-init)
    ;; `input` is bound here because the USB-HID class driver feeds the same
    ;; coreinput service the ps2 keyboard does; the whole bring-up runs in its scope.
    (let ((input (setup-input)))
    ;; Create the compositor owner rendezvous in the ROOT (only the root eval may
    ;; set! a module global) and BEFORE the APs go live, so the per-core shard hook
    ;; can reach it. The bring-up later sends it the owner handle. (set! from here is
    ;; the boot/root context, unlike the bring-up spawn -- see compositor-rendezvous.)
    (set! compositor-rendezvous (make-owner-rendezvous))
    ;; Phase-7 per-core bring-up hook. The single set-per-core-init runs on EVERY
    ;; core as it goes live; it reads (core-id) and, on an AP, brings up that core's
    ;; work. Two gated consumers today: cardinal.percoretest (the cross-core-send
    ;; proof -- an AP messages a BSP `collector`) and cardinal.compositorshards (a
    ;; per-core compositor SHARD that grant-shares a layer the owner merges -- see
    ;; start-compositor-shard). Core 0 hosts the collector / the owner; it spawns no
    ;; AP work for itself. `collector` is #f unless percoretest is on.
    (let ((collector
           (if (cmdline-has? "cardinal.percoretest")
               (let ((expected (- (core-count) 1)))   ; APs that should report (BSP excluded)
                 (spawn-restricted '()
                   (lambda ()
                     (if (<= expected 0)
                         (begin (display "[percoretest] single core; no APs to report") (newline))
                         ;; receive exactly one (core-hello id) per AP, then finish
                         ;; (the context terminates and leaves the BSP run queue).
                         (let loop ((got 0))
                           (let ((m (recv)))
                             (cond ((and (pair? m) (eq? (car m) 'core-hello))
                                    (display "[percoretest] cross-core send OK: core ")
                                    (display (cadr m)) (display " -> BSP collector") (newline)
                                    (if (>= (+ got 1) expected)
                                        (begin (display "[percoretest] all ") (display expected)
                                               (display " AP(s) reported in") (newline))
                                        (loop (+ got 1))))
                                   (else (loop got)))))))) )
               #f)))
      (set-per-core-init
        (lambda ()
          (let ((id (core-id)))                 ; this core's index, read once
            (if (> id 0)
                (begin
                  (if collector
                      (spawn-restricted '() (lambda () (send collector (list 'core-hello id)))))
                  ;; Spawn this core's compositor SHARD. One compositor instance per
                  ;; core is the production model: the owner (core 0) routes opaque
                  ;; clients here so their windows composite on this core, into a
                  ;; grant-shared layer the owner folds into the scanout. Always on --
                  ;; SMP=1 simply has no APs and runs the owner-only N=1 path; the
                  ;; cross-shard tests still get their shards for free.
                  (spawn-restricted '() (lambda () (start-compositor-shard id)))))))))
    ;; Audio: start the service, capture its handle (formerly dropped), and bring
    ;; up the HD Audio controller feeding it. hdaudio-init is gated on pci-find, so
    ;; a default boot with no HDA controller just logs "no device" and returns.
    (let ((audio (start-audio-service)))
      (set! audio-service audio)      ; publish for the REPL `play-tone` command
      ;; Bring up EVERY HD Audio controller, not just the first: pci-find-class-all
      ;; enumerates all class-0x04/0x03 functions, so a machine with two sound cards
      ;; registers both (hda0, hda1, ...). Each gets its own driver context + codec
      ;; enumeration. A default boot with no HDA controller just brings up none.
      (let ((idx 0))
        (for-each
         (lambda (ecam)
           (hdaudio-init audio (string->symbol (string-append "hda" (number->string idx))) ecam)
           (set! idx (+ idx 1)))
         (pci-find-class-all #x04 #x03)))
      ;; Optional capture self-check: start capturing on hda0's first input
      ;; endpoint and confirm the DMA engine runs (see start-mictest).
      (if (cmdline-has? "cardinal.mictest") (start-mictest))
      ;; Optional hotplug exercise: force a codec re-scan, driving the SAME
      ;; reconciliation the state-change interrupt would (scan-all-codecs ->
      ;; re-enumerate -> update the endpoint set). QEMU's hda bus can't actually
      ;; hot-add a codec, so this injects the rescan to validate the path end to end.
      (if (cmdline-has? "cardinal.hotplug")
          (spawn-restricted '()
            (lambda () (sleep 2500000000) (send audio-service (list 'rescan 'hda0)))))
      ;; Optional jack-detect exercise: force a pin-sense re-poll, then report each
      ;; endpoint's presence -- proves the poll read-path runs on real hardware
      ;; (QEMU's sense is static, so nothing changes, but the read must succeed).
      (if (cmdline-has? "cardinal.jacktest")
          (spawn-restricted '()
            (lambda ()
              (sleep 2500000000)
              ;; both messages hit the driver's mailbox in order (single-context
              ;; FIFO at each hop), so the poll is processed before the endpoints
              ;; query -- no sleep needed between them.
              (send audio-service (list 'poll-jacks 'hda0))
              (send audio-service (list 'endpoints 'hda0 (self)))
              (for-each (lambda (d)
                          (display "[jacktest] ep ") (display (car d))
                          (display " ") (display (nth d 2))
                          (display (if (nth d 3) " present" " absent")) (newline))
                        (recv))))))
    (start-power-service)
    ;; Storage registry (the AHCI block driver AND the USB mass-storage class
    ;; driver feed it); cardfs registers as an fs provider FIRST so it is offered
    ;; each block device that registers (a probe reads LBA 0 and claims the volume
    ;; if it is a cardfs superblock). The USB enumeration/dispatch service (coreusb)
    ;; comes up alongside. ahci-init/uhci-init/xhci-init are pci-find-gated, so a
    ;; default boot with no such device just logs "no device" and returns.
    (let ((storage (start-storage-service))
          (usb (start-usb-service)))
      (start-cardfs storage)
      (ahci-init storage)
      ;; Register the USB class drivers FIRST -- coreusb processes these
      ;; register-class messages (sent synchronously here) ahead of any later
      ;; port-connect, so the class table is populated when the first device
      ;; arrives. HID feeds coreinput; mass storage feeds the storage registry;
      ;; the hub recurses through coreusb.
      (usb-hid-init usb input)
      (usb-hub-init usb)
      (usb-storage-init usb storage)
      ;; USB audio feeds the same coreaudio service hdaudio does (audio-service was
      ;; published by the audio bring-up above); a usb-audio device registers as the
      ;; card 'usbaudio0 and streams tones over its iso OUT endpoint.
      (usb-audio-init usb audio-service)
      ;; Then bring up the host controllers.
      (uhci-init usb)
      (xhci-init usb)
      (ehci-init usb))
    ;; Bring up the display registry, then the GPU driver, which brings the
    ;; virtio-gpu device to DRIVER_OK, paints a framebuffer, and registers itself
    ;; with the display service. Guarded: with no GPU present it just logs and
    ;; returns, so a headless smoke boot is unaffected.
    ;; Fallback policy: lfb claims the boot framebuffer (the firmware-provided
    ;; linear framebuffer, e.g. -vga std) ONLY when no virtio-gpu device is on the
    ;; bus. The pci-find gate is race-free -- it tests hardware presence, not a
    ;; registration result -- and matches the C "load lfb only if no display
    ;; registered". This gate would extend (an `or` over their pci-finds) if other
    ;; GPU drivers are added.
    (let* ((demo? (cmdline-has? "cardinal.gfxdemo"))
           (display-svc (start-display-service))
           (gpu (virtio-gpu-init display-svc))
           ;; lfb skips its (slow, uncached-MMIO) bring-up test pattern when the demo
           ;; is about to own the framebuffer, so it registers immediately.
           (disp (if (not (pci-find #x1af4 #x1050)) (lfb-init display-svc (not demo?)) gpu)))
      disp
      ;; With cardinal.gfxdemo set, draw the graphics demo. Prefer the virtio-gpu
      ;; render path when that device is present (compose into a WB-mapped scanout
      ;; backing + virtqueue flush -- the modern path, benchmarkable for real);
      ;; otherwise draw into the WC-mapped boot framebuffer (std-vga) with a
      ;; double-buffer. Same 2D API either way.
      (if demo?
          (if (pci-find #x1af4 #x1050) (start-gpu-demo gpu) (start-gfx-demo)))
      ;; Stand up the multi-client window compositor (notes/servers/CoreCompositor.md).
      ;; Phases 2-4: the IPC root + surface protocol, compositing into a screen
      ;; back-buffer that -- in phase 4 -- IS the real scanout. The kernel authority
      ;; the compositor needs is INJECTED here (init holds it): dma-alloc-wb for
      ;; surface backings, grant-mint/grant-revoke for the zero-copy grants, and a
      ;; `present` closure that pushes composited damage to the display (virtio-gpu
      ;; flush-rects, or a WC-framebuffer copy).
      ;;
      ;; The whole bring-up runs in a SPAWNED context because acquiring the GPU
      ;; scanout (get-framebuffer) blocks until the driver's async bring-up has a
      ;; scanout -- which needs the scheduler to run -- so system-init must not block
      ;; on it. The spawned context starts the service, then exits; the root + its
      ;; per-client handlers live on independently. It holds '(sys-shm) NOT to use
      ;; itself (its scanout map / grant mint are captured prims, which work in any
      ;; context) but so it can DELEGATE sys-shm to the window clients it spawns --
      ;; spawn-restricted refuses to grant a capability the spawner lacks.
      (spawn-restricted '(sys-shm)
        (lambda ()
          (import sys-shm)   ; map-grant: the owner maps per-core shards' layer grants
          (let* ((compdemo? (cmdline-has? "cardinal.compositordemo"))
                 ;; phase 4: own the real scanout for the demo. Otherwise an
                 ;; off-screen RAM screen (the phase-3 posture: composites but
                 ;; displays nothing, inert until a client connects), which also
                 ;; keeps the headless compositortest working.
                 (target (if compdemo?
                             (begin (sleep 800000000)        ; let virtio-gpu bring-up finish
                                    (if (pci-find #x1af4 #x1050)
                                        (compositor-gpu-target gpu)
                                        (compositor-fb-target)))
                             #f))
                 (cw 256) (ch 256)
                 (screen (if target (car target)
                             (make-surface (make-bytes (* cw ch 4)) cw ch (* cw 4))))
                 ;; cardinal.compositordamage: a present that REPORTS the flush rects
                 ;; so the test can confirm a shard's layer-update flushes only its
                 ;; window's damage rect, not the whole screen. It doesn't push pixels
                 ;; (the back-buffer already has them); it only classifies the flush.
                 (present (cond ((cmdline-has? "cardinal.compositordamage")
                                 (lambda (rects)
                                   (if (and (pair? rects) (null? (cdr rects))
                                            (= (nth (car rects) 0) 0) (= (nth (car rects) 1) 0)
                                            (= (nth (car rects) 2) cw) (= (nth (car rects) 3) ch))
                                       (display "[compositordamage] whole-screen flush")
                                       (begin (display "[compositordamage] OK bounded flush ")
                                              (display rects)))
                                   (newline)))
                                (target (cdr target))
                                (else #f)))
                 ;; the 6th cap is the SHARD-MESH KEY: the rendezvous ctx, shared with
                 ;; every shard (and with no client), so the owner can authenticate the
                 ;; privileged inter-instance verbs (register-shard / layer-update /
                 ;; alloc-z) against it -- a client holding `comp` can't forge it.
                 (caps (make-compositor-caps dma-alloc-wb grant-mint grant-revoke present map-grant
                                             compositor-rendezvous))
                 (comp (start-compositor-service screen caps #f)))   ; #f -> owner role
            (send compositor-rendezvous (list 'set-owner comp))   ; publish to per-core shards
            ;; phase 6: the compositor owns focus, so it subscribes to the input
            ;; service -- coreinput now forwards every (input ev) here, and the
            ;; compositor routes each to the focused window's client (keyboard) or
            ;; handles it internally (a title-bar pointer drag MOVES the window).
            ;; This is the real keyboard path: ps2 -> coreinput -> compositor ->
            ;; focused client. `input` is the service handle from setup-input above.
            (send input (list 'subscribe comp))
            (if (and compdemo? (not target))
                (begin (display "[compositor-demo] no display present; nothing to show")
                       (newline)))
            ;; phase-5 demo: TWO independent client contexts each draw their own
            ;; window onto the owned scanout. They overlap (Window Two is staggered
            ;; 400ms so it reliably connects second -> higher z -> on top in the
            ;; overlap; the gap dwarfs a connect/create round-trip but is a timing
            ;; margin, not a protocol guarantee), demonstrating cross-client
            ;; occlusion via the root's single painter's pass.
            (if (and compdemo? target)
                (begin
                  (start-compositor-demo-client comp 0 110 90 360 240 "Window One"
                                                '(245 246 250) '(54 92 168)
                                                '(230 90 70) '(70 170 110))
                  (start-compositor-demo-client comp 400000000 330 250 320 210 "Window Two"
                                                '(250 248 240) '(150 70 140)
                                                '(240 200 70) '(120 110 220))))
            ;; cardinal.compositorinput: exercise phase-6 input routing end to end.
            ;; One client draws a window with a known title-bar colour and places it,
            ;; then INJECTS synthetic events through the input service (QEMU delivers
            ;; no PS2/HID input to the guest, so the events are injected -- the path
            ;; coreinput -> compositor -> client is otherwise identical to a real
            ;; keyboard/mouse). The injected sequence is a title-bar drag (press in
            ;; the title strip -> motion -> release = a window MOVE) then a key.
            ;; Because all four events traverse ONE FIFO (this client -> input ->
            ;; compositor), the key is processed last; the compositor routes it back
            ;; to the focused window (= this client), and its receipt is the
            ;; happens-after BARRIER proving the move completed. The client then
            ;; probes the old vs new title-bar pixel: the old spot now reads the
            ;; desktop background (the window vacated it), the new spot reads the
            ;; title colour (the window moved there). Proves coreinput subscribe/
            ;; fan-out + focus routing + drag-move with no racy cross-context order.
            (if (cmdline-has? "cardinal.compositorinput")
                (spawn-restricted '(sys-shm)
                  (lambda ()
                    (import sys-shm)
                    (send comp (list 'connect #f (self)))
                    (let ((r (recv)))                   ; (connected handler fmt)
                      (if (not (eq? (car r) 'connected))
                          (begin (display "[compositorinput] FAIL no connected reply") (newline))
                          (let* ((h (cadr r)) (fmt (caddr r))
                                 (ro (car fmt)) (go (cadr fmt)) (bo (caddr fmt))
                                 (sw 120) (sh 90))
                            (send h (list 'create-surface sw sh))
                            (let ((s (recv)))           ; (surface id g0 g1 stride)
                              (let* ((id (cadr s)) (g0 (caddr s)) (stride (nth s 4))
                                     (surf (make-surface* (map-grant g0) sw sh stride ro go bo '()))
                                     (title-col (rgb surf 54 92 168))
                                     (bg-col (rgb surf 28 30 44)))
                                (clear surf (rgb surf 245 246 250))
                                (fill-rect surf 0 0 sw 28 title-col)   ; title strip (>= TITLE-H)
                                (send h (list 'configure id 40 40 #t)) ; place at (40,40)
                                (send h (list 'commit id 0 '()))
                                ;; SYNC: configure/commit are relayed via the handler,
                                ;; but the injected events reach the root by another
                                ;; path (input service) and could overtake them. A
                                ;; relayed probe, recv'd, drains the handler->root FIFO
                                ;; so the window is visible/committed before any event.
                                (send h (list 'probe-pixel 0 0))
                                (recv)
                                ;; inject a title-bar drag + a key, all via the input
                                ;; service so coreinput's subscribe/fan-out is exercised.
                                (send input (list 'event (list 'pointer 50 45 #t))) ; press in title (local 10,5)
                                (send input (list 'event (list 'pointer 90 85 #t))) ; drag -> window to (80,80)
                                (send input (list 'event (list 'pointer 90 85 #f))) ; release -> end drag
                                (send input (list 'event (list 'key 30 1)))         ; key -> focused window
                                (let ((ev (recv)))      ; BARRIER: the routed key echo
                                  (display "[compositorinput] focus ")
                                  (display (if (and (pair? ev) (eq? (car ev) 'input)
                                                    (pair? (cadr ev)) (eq? (car (cadr ev)) 'key))
                                               "OK key routed to focused window"
                                               "FAIL key not routed"))
                                  (newline))
                                ;; window moved (40,40)->(80,80); probe old vs new title pixel.
                                (send h (list 'probe-pixel 50 45))   ; old title spot -> background
                                (let ((oldpx (recv)))
                                  (send h (list 'probe-pixel 90 85)) ; new title spot -> title colour
                                  (let ((newpx (recv)))
                                    (display "[compositorinput] move ")
                                    (display (if (and (= oldpx bg-col) (= newpx title-col))
                                                 "OK title-bar drag moved the window"
                                                 "FAIL window did not move"))
                                    (newline)))))))))))
            ;; cardinal.compositorlayers: validate the phase-7 layer/merge pipeline's
            ;; z-buffer occlusion + z authority. One opaque client creates two
            ;; OVERLAPPING windows with distinct solid colours; the later-created one
            ;; has the higher z and must win the overlap (a z-buffer pick, not painter's
            ;; list order). Then `raise` the first -> a fresh top z -> it now wins.
            ;; All ops go through the one handler channel (FIFO), so each probe is
            ;; ordered after the commit/raise before it; probed on the RAM screen.
            ;; cardinal.compositorshards: validate CROSS-CORE CLIENT ROUTING end to
            ;; end. An OPAQUE client connects to the owner; at SMP>1 the owner ROUTES
            ;; it to a shard, so its window composites on that shard's core, into the
            ;; shard's grant-shared layer the owner folds into the scanout. A separate
            ;; TRANSLUCENT verifier connects (translucent stays on the owner) and POLLS
            ;; the owner's scanout at the window position until the colour appears --
            ;; proving the routed client's window crossed cores onto the scanout.
            ;; Polling absorbs the cross-core wake latency. (At SMP=1 there are no
            ;; shards: the opaque client stays on the owner and the test still passes,
            ;; it just isn't exercising a remote shard.)
            (if (cmdline-has? "cardinal.compositorshards")
                (begin
                  ;; the opaque client (routed to a shard): a red 40x40 window at (60,60).
                  (spawn-restricted '(sys-shm)
                    (lambda ()
                      (import sys-shm)
                      (send comp (list 'connect #f (self)))
                      (let ((r (recv)))
                        (if (and (pair? r) (eq? (car r) 'connected))
                            (let* ((h (cadr r)) (fmt (caddr r))
                                   (ro (car fmt)) (go (cadr fmt)) (bo (caddr fmt)))
                              (send h (list 'create-surface 40 40))
                              (let ((s (recv)))
                                (if (and (pair? s) (eq? (car s) 'surface))
                                    (let* ((id (cadr s)) (g0 (caddr s)) (stride (nth s 4))
                                           (surf (make-surface* (map-grant g0) 40 40 stride ro go bo '())))
                                      (clear surf (rgb surf 220 60 60))
                                      (send h (list 'configure id 60 60 #t))
                                      (send h (list 'commit id 0 '()))
                                      (display "[compositorshards] routed client committed a window")
                                      (newline)))))))))
                  ;; the translucent verifier (on the owner): poll the scanout for red.
                  (spawn-restricted '()
                    (lambda ()
                      (send comp (list 'connect #t (self)))
                      (let ((r (recv)))
                        (if (not (and (pair? r) (eq? (car r) 'connected)))
                            (begin (display "[compositorshards] FAIL verifier not connected") (newline))
                            (let* ((h (cadr r)) (fmt (caddr r))
                                   (cs (make-surface* (make-bytes 4) 1 1 4 (car fmt) (cadr fmt) (caddr fmt) '()))
                                   (red (rgb cs 220 60 60)))
                              (let poll ((tries 0))
                                (send h (list 'probe-pixel 75 75))
                                (let ((got (recv)))
                                  (cond ((and (integer? got) (= got red))
                                         (display "[compositorshards] OK routed client window reached the scanout")
                                         (newline))
                                        ((>= tries 120)
                                         (display "[compositorshards] FAIL window never reached the scanout (timeout)")
                                         (newline))
                                        (else (sleep 100000000) (poll (+ tries 1)))))))))))))
            ;; cardinal.compositorshardinput: validate CROSS-SHARD KEYBOARD FOCUS. An
            ;; opaque client is routed to a shard (SMP>1) and shows a window, so the
            ;; shard reports it to the owner as its focus candidate. The owner (the
            ;; coreinput subscriber + focus authority) must then route an INJECTED key
            ;; to that client even though it lives on another core. The client parks on
            ;; recv; an injector feeds keys into coreinput (repeated, to outlast the
            ;; cross-core focus-report latency); receipt proves end-to-end cross-shard
            ;; keyboard routing. (At SMP=1 the client is owner-hosted and it still works.)
            (if (cmdline-has? "cardinal.compositorshardinput")
                (begin
                  (spawn-restricted '(sys-shm)
                    (lambda ()
                      (import sys-shm)
                      (wait-for-shards comp)   ; route to a shard deterministically
                      (send comp (list 'connect #f (self)))
                      (let ((r (recv)))
                        (if (and (pair? r) (eq? (car r) 'connected))
                            (let* ((h (cadr r)) (fmt (caddr r))
                                   (ro (car fmt)) (go (cadr fmt)) (bo (caddr fmt)))
                              (send h (list 'create-surface 40 40))
                              (let ((s (recv)))
                                (if (and (pair? s) (eq? (car s) 'surface))
                                    (let* ((id (cadr s)) (g0 (caddr s)) (stride (nth s 4))
                                           (surf (make-surface* (map-grant g0) 40 40 stride ro go bo '())))
                                      (clear surf (rgb surf 220 60 60))
                                      (send h (list 'configure id 60 60 #t))
                                      (send h (list 'commit id 0 '()))
                                      ;; wait for a key routed to us (we are the focus),
                                      ;; with a deadline so a routing failure prints FAIL
                                      ;; rather than hanging the CI run silently. Poll the
                                      ;; mailbox (recv would block past the deadline).
                                      (let loop ((deadline (+ (uptime-ns) 16000000000)))
                                        (cond ((> (uptime-ns) deadline)
                                               (display "[compositorshardinput] FAIL key never delivered (timeout)")
                                               (newline))
                                              ((%mailbox-empty?) (sleep 100000000) (loop deadline))
                                              (else
                                               (let ((m (%mailbox-pop)))
                                                 (if (and (pair? m) (eq? (car m) 'input)
                                                          (pair? (cadr m)) (eq? (car (cadr m)) 'key))
                                                     (begin (display "[compositorshardinput] OK shard-hosted client received key")
                                                            (newline))
                                                     (loop deadline))))))))))))))
                  (spawn-restricted '()
                    (lambda ()
                      ;; feed keys into coreinput repeatedly; the owner forwards each to
                      ;; the focused window (this routes once the shard has reported it).
                      (let loop ((n 0))
                        (if (< n 20)
                            (begin (sleep 600000000)
                                   (send input (list 'event (list 'key 30 1)))
                                   (loop (+ n 1)))))))))
            ;; cardinal.compositorshardpointer: validate CROSS-SHARD POINTER routing.
            ;; Same shape as shardinput, but the injector feeds a POINTER body-click
            ;; over the shard-hosted window; the owner hit-tests the global window
            ;; manifest, finds the shard window, and routes the press (in window-local
            ;; coords) to its client on another core. (Window is 40x40 at (60,60);
            ;; (75,92) is in the body, local (15,32).)
            (if (cmdline-has? "cardinal.compositorshardpointer")
                (begin
                  (spawn-restricted '(sys-shm)
                    (lambda ()
                      (import sys-shm)
                      (wait-for-shards comp)   ; route to a shard deterministically
                      (send comp (list 'connect #f (self)))
                      (let ((r (recv)))
                        (if (and (pair? r) (eq? (car r) 'connected))
                            (let* ((h (cadr r)) (fmt (caddr r))
                                   (ro (car fmt)) (go (cadr fmt)) (bo (caddr fmt)))
                              (send h (list 'create-surface 40 40))
                              (let ((s (recv)))
                                (if (and (pair? s) (eq? (car s) 'surface))
                                    (let* ((id (cadr s)) (g0 (caddr s)) (stride (nth s 4))
                                           (surf (make-surface* (map-grant g0) 40 40 stride ro go bo '())))
                                      (clear surf (rgb surf 220 60 60))
                                      (send h (list 'configure id 60 60 #t))
                                      (send h (list 'commit id 0 '()))
                                      (let loop ((deadline (+ (uptime-ns) 16000000000)))
                                        (cond ((> (uptime-ns) deadline)
                                               (display "[compositorshardpointer] FAIL pointer never delivered (timeout)")
                                               (newline))
                                              ((%mailbox-empty?) (sleep 100000000) (loop deadline))
                                              (else
                                               (let ((m (%mailbox-pop)))
                                                 (if (and (pair? m) (eq? (car m) 'input)
                                                          (pair? (cadr m)) (eq? (car (cadr m)) 'pointer))
                                                     (begin (display "[compositorshardpointer] OK shard-hosted client received pointer")
                                                            (newline))
                                                     (loop deadline))))))))))))))
                  (spawn-restricted '()
                    (lambda ()
                      (let loop ((n 0))
                        (if (< n 20)
                            (begin (sleep 600000000)
                                   (send input (list 'event (list 'pointer 75 92 #t)))
                                   (loop (+ n 1)))))))))
            ;; cardinal.compositorsharddrag: validate CROSS-SHARD DRAG-MOVE. A routed
            ;; shard-hosted window is dragged by its title bar; the owner detects the
            ;; press over the shard window, then relays each motion to the hosting
            ;; shard (move-window), which repositions it and re-merges. An injector
            ;; feeds a title-bar drag (press 70,65 -> motion 120,115 -> release) that
            ;; should move the 40x40 window from (60,60) to (110,110); a translucent
            ;; verifier polls the scanout at the NEW window centre (130,130) for the
            ;; window colour. The drag is injected a few times so an early attempt
            ;; (manifest not yet reported) is retried; once moved, later presses miss.
            (if (cmdline-has? "cardinal.compositorsharddrag")
                (begin
                  (spawn-restricted '(sys-shm)
                    (lambda ()
                      (import sys-shm)
                      (wait-for-shards comp)   ; route to a shard deterministically (not a race)
                      (send comp (list 'connect #f (self)))
                      (let ((r (recv)))
                        (if (and (pair? r) (eq? (car r) 'connected))
                            (let* ((h (cadr r)) (fmt (caddr r))
                                   (ro (car fmt)) (go (cadr fmt)) (bo (caddr fmt)))
                              (send h (list 'create-surface 40 40))
                              (let ((s (recv)))
                                (if (and (pair? s) (eq? (car s) 'surface))
                                    (let* ((id (cadr s)) (g0 (caddr s)) (stride (nth s 4))
                                           (surf (make-surface* (map-grant g0) 40 40 stride ro go bo '())))
                                      (clear surf (rgb surf 220 60 60))
                                      (send h (list 'configure id 60 60 #t))
                                      (send h (list 'commit id 0 '()))
                                      (let loop () (recv) (loop))))))))))   ; park to keep the window alive
                  (spawn-restricted '()
                    (lambda ()
                      ;; inject the title-bar drag a few times (retry until the manifest is up).
                      (let loop ((n 0))
                        (if (< n 5)
                            (begin (sleep 3000000000)
                                   (send input (list 'event (list 'pointer 70 65 #t)))    ; press in title
                                   (send input (list 'event (list 'pointer 120 115 #t)))  ; drag -> (110,110)
                                   (send input (list 'event (list 'pointer 120 115 #f)))  ; release
                                   (loop (+ n 1)))))))
                  (spawn-restricted '()
                    (lambda ()
                      (send comp (list 'connect #t (self)))   ; translucent -> owner; probe the scanout
                      (let ((r (recv)))
                        (if (and (pair? r) (eq? (car r) 'connected))
                            (let* ((h (cadr r)) (fmt (caddr r))
                                   (cs (make-surface* (make-bytes 4) 1 1 4 (car fmt) (cadr fmt) (caddr fmt) '()))
                                   (red (rgb cs 220 60 60)))
                              (let poll ((tries 0))
                                (send h (list 'probe-pixel 130 130))   ; the moved window's centre
                                (let ((got (recv)))
                                  (cond ((and (integer? got) (= got red))
                                         (display "[compositorsharddrag] OK shard window dragged across the scanout")
                                         (newline))
                                        ((>= tries 200)
                                         (display "[compositorsharddrag] FAIL window did not move (timeout)")
                                         (newline))
                                        (else (sleep 100000000) (poll (+ tries 1)))))))))))))
            ;; cardinal.compositorglobalz: validate TRUE GLOBAL Z across shards. Two
            ;; opaque clients route to DIFFERENT shards (round-robin) with OVERLAPPING
            ;; windows: A (red) created first, B (green) created later -> B on top. Then
            ;; A RAISES -- with the old static per-shard z-bands A (on a lower-id shard)
            ;; could NEVER rise above B, but with the owner's global z counter A's raise
            ;; gives it the highest z and it goes on top. A translucent verifier polls
            ;; the overlap (90,90) for RED (A on top after the raise).
            (if (cmdline-has? "cardinal.compositorglobalz")
                (begin
                  (spawn-restricted '(sys-shm)        ; A (red) -> shard 0; raises after B is up
                    (lambda ()
                      (import sys-shm)
                      (wait-for-shards comp)
                      (send comp (list 'connect #f (self)))
                      (let ((r (recv)))
                        (if (and (pair? r) (eq? (car r) 'connected))
                            (let* ((h (cadr r)) (fmt (caddr r)) (ro (car fmt)) (go (cadr fmt)) (bo (caddr fmt)))
                              (send h (list 'create-surface 60 60))
                              (let ((s (recv)))
                                (if (and (pair? s) (eq? (car s) 'surface))
                                    (let* ((id (cadr s)) (g0 (caddr s)) (stride (nth s 4))
                                           (surf (make-surface* (map-grant g0) 60 60 stride ro go bo '())))
                                      (clear surf (rgb surf 220 60 60))
                                      (send h (list 'configure id 50 50 #t))
                                      (send h (list 'commit id 0 '()))
                                      (sleep 5000000000)             ; let B come up (higher z) first
                                      (send h (list 'raise id))      ; cross-shard raise above B
                                      (let loop () (recv) (loop))))))))))
                  (spawn-restricted '(sys-shm)        ; B (green) -> shard 1 (connects after A)
                    (lambda ()
                      (import sys-shm)
                      (wait-for-shards comp)
                      (sleep 2000000000)
                      (send comp (list 'connect #f (self)))
                      (let ((r (recv)))
                        (if (and (pair? r) (eq? (car r) 'connected))
                            (let* ((h (cadr r)) (fmt (caddr r)) (ro (car fmt)) (go (cadr fmt)) (bo (caddr fmt)))
                              (send h (list 'create-surface 60 60))
                              (let ((s (recv)))
                                (if (and (pair? s) (eq? (car s) 'surface))
                                    (let* ((id (cadr s)) (g0 (caddr s)) (stride (nth s 4))
                                           (surf (make-surface* (map-grant g0) 60 60 stride ro go bo '())))
                                      (clear surf (rgb surf 70 170 110))
                                      (send h (list 'configure id 80 80 #t))
                                      (send h (list 'commit id 0 '()))
                                      (let loop () (recv) (loop))))))))))
                  (spawn-restricted '()               ; verifier: overlap pixel must become RED
                    (lambda ()
                      (send comp (list 'connect #t (self)))
                      (let ((r (recv)))
                        (if (and (pair? r) (eq? (car r) 'connected))
                            (let* ((h (cadr r)) (fmt (caddr r))
                                   (cs (make-surface* (make-bytes 4) 1 1 4 (car fmt) (cadr fmt) (caddr fmt) '()))
                                   (red (rgb cs 220 60 60)))
                              (let poll ((tries 0))
                                (send h (list 'probe-pixel 90 90))   ; overlap of A and B
                                (let ((got (recv)))
                                  (cond ((and (integer? got) (= got red))
                                         (display "[compositorglobalz] OK cross-shard raise put the window on top")
                                         (newline))
                                        ((>= tries 250)
                                         (display "[compositorglobalz] FAIL window not raised above (timeout)")
                                         (newline))
                                        (else (sleep 100000000) (poll (+ tries 1)))))))))))))
            ;; cardinal.compositordamage: validate LAYER-UPDATE DAMAGE BOUNDING. A
            ;; client routed to a shard creates+commits a 40x40 window at (60,60); the
            ;; shard's layer-update carries that damage rect, and the owner flushes ONLY
            ;; it (not the whole 256x256 screen) -- the detecting `present` above prints
            ;; "OK bounded flush ((60 60 40 40))" for it (vs the startup whole-screen).
            (if (cmdline-has? "cardinal.compositordamage")
                (spawn-restricted '(sys-shm)
                  (lambda ()
                    (import sys-shm)
                    ;; only the SHARD layer-update path bounds the flush (the owner's own
                    ;; ops already flush bounded); with no shards there's nothing to test.
                    (if (<= (wait-for-shards comp) 0)
                        (begin (display "[compositordamage] single core; shard flush path not exercised")
                               (newline)))
                    (send comp (list 'connect #f (self)))
                    (let ((r (recv)))
                      (if (and (pair? r) (eq? (car r) 'connected))
                          (let* ((h (cadr r)) (fmt (caddr r))
                                 (ro (car fmt)) (go (cadr fmt)) (bo (caddr fmt)))
                            (send h (list 'create-surface 40 40))
                            (let ((s (recv)))
                              (if (and (pair? s) (eq? (car s) 'surface))
                                  (let* ((id (cadr s)) (g0 (caddr s)) (stride (nth s 4))
                                         (surf (make-surface* (map-grant g0) 40 40 stride ro go bo '())))
                                    (clear surf (rgb surf 220 60 60))
                                    (send h (list 'configure id 60 60 #t))
                                    (send h (list 'commit id 0 '()))
                                    (let loop () (recv) (loop)))))))))))   ; park
            (if (cmdline-has? "cardinal.compositorlayers")
                (spawn-restricted '(sys-shm)
                  (lambda ()
                    (import sys-shm)
                    (send comp (list 'connect #f (self)))
                    (let ((r (recv)))
                      (if (not (eq? (car r) 'connected))
                          (begin (display "[compositorlayers] FAIL no connected reply") (newline))
                          (let* ((h (cadr r)) (fmt (caddr r))
                                 (ro (car fmt)) (go (cadr fmt)) (bo (caddr fmt))
                                 (cs (make-surface* (make-bytes 4) 1 1 4 ro go bo '())) ; rgb in screen fmt
                                 (red (rgb cs 255 0 0)) (green (rgb cs 0 255 0))
                                 (mkwin (lambda (w hh x y col)
                                          (send h (list 'create-surface w hh))
                                          (let ((s (recv)))
                                            (if (not (and (pair? s) (eq? (car s) 'surface)))
                                                (begin (display "[compositorlayers] FAIL create-surface") (newline) #f)
                                                (let* ((id (cadr s)) (g0 (caddr s)) (stride (nth s 4))
                                                       (surf (make-surface* (map-grant g0) w hh stride ro go bo '())))
                                                  (clear surf col)
                                                  (send h (list 'configure id x y #t))
                                                  (send h (list 'commit id 0 '()))
                                                  id)))))
                                 (probe (lambda (x y) (send h (list 'probe-pixel x y)) (recv))))
                            (let ((ida (mkwin 40 40 20 20 red))     ; z=1, [20,60)x[20,60)
                                  (idb (mkwin 40 40 40 40 green)))  ; z=2, [40,80)x[40,80) -> on top
                              (display "[compositorlayers] zorder ")
                              (display (if (and (= (probe 45 45) green)  ; overlap -> higher z
                                                (= (probe 25 25) red)    ; A only
                                                (= (probe 70 70) green)) ; B only
                                           "OK higher-z window wins the overlap"
                                           "FAIL z-buffer occlusion wrong"))
                              (newline)
                              (send h (list 'raise ida))             ; A gets a fresh top z
                              (display "[compositorlayers] raise ")
                              (display (if (= (probe 45 45) red)
                                           "OK raise lifts the window above in z"
                                           "FAIL raise did not change occlusion"))
                              (newline))))))))
            ;; cardinal.compositortest: a real end-to-end surface round-trip under the
            ;; kernel scheduler -- connect, create a surface, map its grant (zero-copy),
            ;; draw a known colour, commit, then probe the composited screen. The client
            ;; is spawn-restricted to '(sys-shm) so it can map ONLY granted memory (the
            ;; window-client posture), proving map-grant works from a restricted ctx.
            ;; The host test_compositor covers paint-windows + the protocol mechanics.
            (if (cmdline-has? "cardinal.compositortest")
                (spawn-restricted '(sys-shm)
                  (lambda ()
                    (import sys-shm)
                    (send comp (list 'connect #f (self)))
                    (let ((r (recv)))                   ; (connected handler fmt)
                      (if (not (eq? (car r) 'connected))
                          (begin (display "[compositor-test] FAIL no connected reply") (newline))
                          (let ((h (cadr r)))
                            (send h (list 'create-surface 4 4))   ; reply comes to us
                            (let ((s (recv)))           ; (surface id g0 g1 stride)
                              (let* ((id (cadr s)) (g0 (caddr s)) (stride (nth s 4))
                                     (surf (make-surface (map-grant g0) 4 4 stride))
                                     (red (rgb surf 255 0 0)))
                                (fill-rect surf 0 0 4 4 red) ; draw into back buffer (front=0=g0)
                                (send h (list 'configure id 10 10 #t))
                                (send h (list 'commit id 0 '()))
                                ;; probe via the handler so it is ordered after commit.
                                (send h (list 'probe-pixel 10 10))
                                (let ((px (recv)))
                                  (display "[compositor-test] ")
                                  (display (if (= px red) "OK surface created, drawn, composited"
                                               "FAIL composited pixel mismatch"))
                                  (newline))
                                ;; Use-after-revoke (the zero-page hardening): the
                                ;; client still holds `surf` (its mapped g0 view). Read
                                ;; it live (red), then destroy the surface -- which
                                ;; revokes g0/g1 -- and read the SAME view again. A
                                ;; revoked grant reads as a zero page, so the late read
                                ;; returns 0, never the (reused) backing RAM. This drives
                                ;; the real map-grant token-stamp wiring end to end (the
                                ;; host test stubs that step).
                                (let ((live-px (get-pixel surf 0 0)))
                                  (send h (list 'destroy-surface id))
                                  (recv)                ; 'ok ack, sent after revoke
                                  (let ((dead-px (get-pixel surf 0 0)))
                                    (display "[compositor-test] revoke ")
                                    (display (if (and (= live-px red) (= dead-px 0))
                                                 "OK use-after-revoke reads zero"
                                                 "FAIL revoked view did not zero"))
                                    (newline)))))))))))))))
    ;; Bring up the network stack, then a NIC, which registers itself with the
    ;; stack and forwards frames to it. Prefer the proven virtio-net when present;
    ;; otherwise fall back to the rtl8139 (the `-device rtl8139` boot, where no
    ;; virtio-net exists). The register-nic the NIC sends sits ahead of the
    ;; messages below in the service's mailbox, so the MAC/TX are set first.
    ;;
    ;; Addressing policy (DHCP is the default; cardinal.ip=A.B.C.D forces a static
    ;; address and skips it): a static boot comes up at the pinned IP and primes
    ;; the ARP cache with a who-has for the slirp gateway -- the reply exercises
    ;; NIC RX -> service demux -> ARP cache end to end. A default boot comes up at
    ;; 0.0.0.0 and sends dhcp-start; the DHCP client acquires IP/mask/gw/dns and
    ;; learns the gateway MAC itself, so no manual prime is wanted there.
    (let* ((static (static-ip))
           (net (start-network-service)))
      ;; Bring up EVERY supported NIC, not just the first: pci-find-all enumerates
      ;; all matching devices (so two virtio-nets, or a virtio-net + an rtl, both
      ;; come up), and each driver-init registers its device as its own interface.
      ;; `bring-up` runs a driver over each ecam of a (vid did) and counts NICs.
      (let ((nics 0))
        (define (bring-up vid did drv)
          (for-each (lambda (ecam) (drv net ecam) (set! nics (+ nics 1)))
                    (pci-find-all vid did)))
        (bring-up #x1af4 #x1041 virtio-net-init)
        (bring-up #x10ec #x8168 rtl8169-init)
        (bring-up #x10ec #x8139 rtl8139-init)
        (if (= nics 0) (begin (display "[init] no supported NIC") (newline))))
      ;; The register-nic each NIC sends sits ahead of these in the service mailbox,
      ;; so every interface exists before we address it. Static: assign the pinned
      ;; IP on a /24 to the primary interface (mac #f) and prime the gw who-has.
      ;; Default: DHCP on EVERY interface (dhcp-start-all); each client configures
      ;; its own interface (the OFFER/ACK fan out on port 68, filtered by xid/MAC).
      (if static
          (begin
            (send net (list 'set-address #f static (list 255 255 255 0) (list 0 0 0 0) (list 0 0 0 0)))
            (send net (list 'arp-request (list 10 0 2 2))))
          (send net (list 'dhcp-start-all)))                ; default: configure via DHCP
      ;; Optional network debug endpoint (echo 1337 / digest 1338), gated on the
      ;; kernel command line -- a remote attack surface, so opt-in like the REPL.
      (if (cmdline-has? "cardinal.netdbg") (start-netdebug net))
      ;; Test-only TCP fault injection: cardinal.tcploss makes the stack drop 1 in
      ;; 4 received segments, exercising retransmission + out-of-order reassembly.
      (if (cmdline-has? "cardinal.tcploss") (send net (list 'tcp-test-loss 4)))
      ;; Test-only firewall demo: cardinal.fwtest installs a rule that DENIES
      ;; inbound TCP to port 7, so the netdbg TCP echo becomes unreachable while
      ;; everything else (DHCP/DNS/UDP echo) still works -- a visible deny.
      (if (cmdline-has? "cardinal.fwtest")
          (send net (list 'fw-add 'deny 6 'any 0 7)))
      ;; Test-only DNS probe: resolve a hostname via the DHCP-learned server and
      ;; print the result (after DHCP has had time to bind the interface).
      (if (cmdline-has? "cardinal.dnstest")
          (spawn-restricted '()
            (lambda ()
              (sleep 3000000000)
              (display "[dnstest] example.com -> ")
              (display (dns-resolve net "example.com")) (newline)))))
    'system-up))   ; close the (let ((input ...)) ...) that wraps the bring-up

  ;; The interactive serial REPL (started only under cardinal.repl). A root context
  ;; -- a serial shell for debugging the OS is exactly the case that wants full
  ;; authority -- it imports the console I/O + the ISA-IRQ bridge, claims COM1 RX
  ;; (IRQ 4), arms the UART receive interrupt, then PARKS on the line: an arriving
  ;; byte wakes it, it drains + evaluates the input and writes the transcript back,
  ;; then parks again. No busy-polling -- the BSP idles between keystrokes. `seen`
  ;; is captured before each drain so a byte landing mid-drain is never lost (the
  ;; count-based wake, same as the ps2 pump).
  (define (start-repl)
    (spawn (lambda ()
      (import sys-console sys-irq)
      (let ((irq (irq-register 4)))               ; COM1 receive = ISA IRQ 4
        (if (not irq)
            (begin (display "[repl] irq-register failed") (newline))
            (begin
              (console-arm-rx)                     ; enable COM1 RX IRQ now it is routed
              ;; Expose init's REPL command(s) in the REPL's persistent env so they
              ;; can be called bare, e.g. (play-tone 440). repl-eval evaluates in
              ;; that env; init is already loaded, so this just binds the export
              ;; (idempotent), it does not re-run system-init. The (only play-tone)
              ;; clause is deliberate: importing all of init would also expose
              ;; system-init / start-repl, which a stray REPL call could use to
              ;; re-run the whole boot or spawn a second REPL that steals COM1.
              (repl-eval "(import (init (only play-tone set-vol)))")
              (display "[repl] serial REPL ready on COM1 -- try (play-tone)") (newline)
              (console-flush)                        ; log is batched -- push the banner out now
              (let loop ((seen (irq-count irq)))
                (let ((in (console-poll)))
                  (if in
                      (begin (console-write (repl-eval in)) (console-flush) (loop (irq-count irq)))
                      (begin (irq-wait irq seen) (loop (irq-count irq)))))))))))) ) ; last ) closes (define-module init ...)
