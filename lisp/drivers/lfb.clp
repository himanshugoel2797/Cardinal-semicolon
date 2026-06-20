;; lfb: the linear-framebuffer display driver, ported from drivers/lfb/src/main.c.
;;
;; The boot firmware (multiboot2/GRUB) hands the kernel a pre-configured linear
;; framebuffer; SysReg records its geometry under HW/BOOTINFO/FRAMEBUFFER. This
;; driver reads those keys, maps the framebuffer MMIO, and registers a "Linear
;; Framebuffer" display with coredisplay -- the fallback display when no GPU
;; driver claimed the bus. Unlike virtio-gpu there is no device handshake: the
;; framebuffer is already live and directly scanned out, so (flush) is a no-op
;; and there is no DMA/IRQ -- only sys-reg (the geometry) and sys-mmio (the
;; mapping). It is therefore the narrowest-capability display driver in the tree.
;;
;; The C driver only registered the handler table; this port additionally paints
;; a recognizable test pattern on bring-up, both as the live proof that the
;; mapping reaches real scanned-out pixels and as a stand-in first frame. A real
;; desktop would clear/own the framebuffer once it took over (see fb-test-pattern!).

(define-module lfb
  (export lfb-init pack-rgb lfb-register-msg)
  (import sys-reg sys-mmio driver-util)

  (define FB-PATH "HW/BOOTINFO/FRAMEBUFFER")
  (define BYTES-PER-PIXEL 4)

  ;; --- geometry ---------------------------------------------------------------
  ;; Read PHYS_ADDR/PITCH/WIDTH/HEIGHT (+ the colour-channel bit offsets) from the
  ;; boot framebuffer record. Returns a positional list
  ;;   (phys pitch width height r-off g-off b-off bpp)
  ;; or #f if any of the four geometry keys is missing or zero (no usable boot
  ;; framebuffer -- the caller logs and skips registration, exactly as the C
  ;; server only loaded lfb when no display had registered).
  ;;
  ;; The channel offsets default to the conventional X8R8G8B8 layout (16/8/0) when
  ;; absent; bpp defaults to 32 (QEMU std-vga at our gfxmode), with the pitch/width
  ;; ratio used only as a sanity fallback. There is deliberately no BPP registry
  ;; key (see SysReg/bootinfo.c -- it writes the masks/offsets but no bpp).
  (define (reg-or path key dflt)
    (let ((v (reg-read-uint path key)))
      (if v v dflt)))

  (define (fb-params)
    (let ((phys   (reg-read-uint FB-PATH "PHYS_ADDR"))
          (pitch  (reg-read-uint FB-PATH "PITCH"))
          (width  (reg-read-uint FB-PATH "WIDTH"))
          (height (reg-read-uint FB-PATH "HEIGHT")))
      (if (or (not phys) (not pitch) (not width) (not height)
              (= phys 0) (= pitch 0) (= width 0) (= height 0))
          #f
          (let ((r-off (reg-or FB-PATH "RED_OFFSET" 16))
                (g-off (reg-or FB-PATH "GREEN_OFFSET" 8))
                (b-off (reg-or FB-PATH "BLUE_OFFSET" 0))
                ;; sanity fallback only; default to 32.
                (bpp   (if (> width 0) (quotient (* 8 pitch) width) 32)))
            (list phys pitch width height r-off g-off b-off
                  (if (> bpp 0) bpp 32))))))

  ;; --- pixel packing (pure, exported, unit-tested) ----------------------------
  ;; Assemble an R8G8B8 colour into the framebuffer's pixel word given the channel
  ;; bit offsets. For the usual 16/8/0 layout this is the familiar 0x00RRGGBB.
  (define (pack-rgb r g b r-off g-off b-off)
    (bitwise-or (arithmetic-shift r r-off)
                (arithmetic-shift g g-off)
                (arithmetic-shift b b-off)))

  ;; --- mapping + painting -----------------------------------------------------
  ;; Map the full byte extent of the framebuffer (pitch*height -- matching the C
  ;; vmem_phystovirt size), uncached and u32-writable.
  (define (map-fb phys pitch height)
    (mmio-map phys (* pitch height)))

  ;; Write one 32-bit pixel at (x,y). pitch is in bytes, so the row stride is
  ;; pitch and the column stride is BYTES-PER-PIXEL.
  (define (fb-put-pixel! fb pitch x y color)
    (bytes-u32-set! fb (+ (* y pitch) (* x BYTES-PER-PIXEL)) color))

  ;; Paint a recognizable smooth gradient: red increases across X, green down Y,
  ;; blue held constant at 128. Chosen so a sampled pixel's expected colour is
  ;; COMPUTABLE for the live screenshot check:
  ;;   pixel(x,y) = pack-rgb (x mod 256) (y mod 256) 128
  ;; A real desktop would clear/own the framebuffer; this is the bring-up proof.
  ;;
  ;; This is millions of interpreted iterations, so the per-pixel work is kept
  ;; minimal: the green/blue contribution (constant across a row) and the row's
  ;; base byte offset are hoisted out of the inner loop, and the inner loop tracks
  ;; the byte offset incrementally rather than recomputing (* y pitch)+(* x 4).
  (define (fb-test-pattern! fb width height pitch r-off g-off b-off)
    (let yloop ((y 0))
      (if (< y height)
          (let ((gb (bitwise-or (arithmetic-shift (modulo y 256) g-off)
                                (arithmetic-shift 128 b-off)))
                (row (* y pitch)))
            (let xloop ((x 0) (off row))
              (if (< x width)
                  (begin
                    (bytes-u32-set! fb off
                                    (bitwise-or (arithmetic-shift (modulo x 256) r-off) gb))
                    (xloop (+ x 1) (+ off BYTES-PER-PIXEL)))))
            (yloop (+ y 1)))
          fb)))

  ;; Fill the whole framebuffer with one colour (the (fill color) message).
  (define (fb-fill! fb width height pitch color)
    (let yloop ((y 0))
      (if (< y height)
          (begin
            (let xloop ((x 0))
              (if (< x width)
                  (begin (fb-put-pixel! fb pitch x y color) (xloop (+ x 1)))))
            (yloop (+ y 1)))
          fb)))

  ;; --- registration (factored out so the self-test can drive it hardware-free) -
  ;; Send the coredisplay register message for a brought-up framebuffer. coredisplay
  ;; stores (cdr m) verbatim, so we hand it (register name conn ctx info); info is
  ;; (width height pitch bpp). `ctx` is the driver loop's own handle so consumers
  ;; can send it get-framebuffer/get-displayinfo/get-status/flush/fill.
  (define (lfb-register-msg display-svc width height pitch ctx)
    (send display-svc
          (list 'register "Linear Framebuffer" 'unknown ctx
                (list width height pitch 32))))

  ;; The long-lived driver context. Mirrors the virtio-gpu serial recv loop, but
  ;; the framebuffer is directly scanned out so most requests are pure reads.
  ;;
  ;; The test-pattern paint runs HERE, at the top of the spawned context, NOT in
  ;; lfb-init's root caller: a 1080p paint is millions of iterations, and only a
  ;; scheduled context's per-context heap is GC-collected mid-loop -- painting in
  ;; the root init context (a direct eval, never GC'd) would exhaust the system
  ;; heap. A desktop would clear/own the framebuffer once it took over.
  (define (lfb-driver-loop display-svc fb width height pitch r-off g-off b-off)
    (fb-test-pattern! fb width height pitch r-off g-off b-off)
    (lfb-register-msg display-svc width height pitch (self))
    (let loop ()
      (let ((m (recv)))
        (cond
          ((eq? (car m) 'get-framebuffer)
           (send (cadr m) (list fb width height pitch)))
          ((eq? (car m) 'get-displayinfo)
           (send (cadr m) (list width height pitch 32 60)))
          ((eq? (car m) 'get-status)
           (send (cadr m) 'connected))
          ;; The linear framebuffer is directly scanned out by the display
          ;; controller -- there is nothing to push to a host, so flush is a no-op.
          ((eq? (car m) 'flush) 'noop)
          ((eq? (car m) 'fill)
           (fb-fill! fb width height pitch (cadr m)))
          (else 'ignored))
        (loop))))

  ;; The entry point. `display-svc` is the coredisplay service handle. Read the
  ;; boot-framebuffer geometry; if absent, log and return #f (no fallback display).
  ;; Otherwise map the framebuffer and spawn the long-lived restricted driver
  ;; context, which paints the test pattern (the live proof -- on its own GC'd
  ;; heap, NOT the root init heap), registers with coredisplay, and serves display
  ;; requests. Returns the spawned handle (or #f when there is no boot framebuffer).
  (define (lfb-init display-svc)
    (let ((p (fb-params)))
      (if (not p)
          (begin (display "[lfb] no boot framebuffer; not registering") (newline) #f)
          (let ((phys   (nth p 0)) (pitch  (nth p 1))
                (width  (nth p 2)) (height (nth p 3))
                (r-off  (nth p 4)) (g-off  (nth p 5)) (b-off  (nth p 6)))
            (let ((fb (map-fb phys pitch height)))
              (display "[lfb] up: ") (display width) (display "x") (display height)
              (display " @ pitch ") (display pitch) (newline)
              (spawn-restricted '()
                (lambda ()
                  (lfb-driver-loop display-svc fb width height pitch
                                   r-off g-off b-off)))))))))
