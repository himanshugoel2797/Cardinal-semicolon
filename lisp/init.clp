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

  ;; The system entry point: called once on the BSP after the scheduler is live.
  ;; Each Core* service is a long-lived context; bring up the ones that exist
  ;; today (input, audio, power) and the NIC. Audio/power have no drivers feeding
  ;; them yet -- they idle parked on recv -- but the endpoints are present for the
  ;; drivers that will attach (the same posture the C servers held).
  (define (system-init)
    ;; `input` is bound here because the USB-HID class driver feeds the same
    ;; coreinput service the ps2 keyboard does; the whole bring-up runs in its scope.
    (let ((input (setup-input)))
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
      ;; Phase 3: the IPC root + the surface protocol (create/configure/commit/
      ;; destroy) compositing into a screen back-buffer. The kernel authority the
      ;; root needs is INJECTED here (init holds it): dma-alloc-wb for surface
      ;; backings + grant-mint/grant-revoke for the zero-copy grants. The screen is
      ;; a RAM back-buffer for now -- phase 4 swaps it for the real scanout and adds
      ;; the flush-rects path; until then nothing is displayed, so the bring-up is
      ;; inert unless a client connects.
      (let* ((cw 256) (ch 256)
             (screen (make-surface (make-bytes (* cw ch 4)) cw ch (* cw 4)))
             (caps (make-compositor-caps dma-alloc-wb grant-mint grant-revoke))
             (comp (start-compositor-service screen caps)))
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
                (let ((r (recv)))                       ; (connected handler)
                  (if (not (eq? (car r) 'connected))
                      (begin (display "[compositor-test] FAIL no connected reply") (newline))
                      (let ((h (cadr r)))
                        (send h (list 'create-surface 4 4))   ; reply comes to us
                        (let ((s (recv)))               ; (surface id g0 g1 stride)
                          (let* ((id (cadr s)) (g0 (caddr s)) (stride (nth s 4))
                                 (surf (make-surface (map-grant g0) 4 4 stride))
                                 (red (rgb surf 255 0 0)))
                            (fill-rect surf 0 0 4 4 red)  ; draw into back buffer (front=0=g0)
                            (send h (list 'configure id 10 10 #t))
                            (send h (list 'commit id 0 '()))
                            ;; probe via the handler so it is ordered after commit.
                            (send h (list 'probe-pixel 10 10))
                            (let ((px (recv)))
                              (display "[compositor-test] ")
                              (display (if (= px red) "OK surface created, drawn, composited"
                                           "FAIL composited pixel mismatch"))
                              (newline))))))))))))
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
