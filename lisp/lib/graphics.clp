;; graphics: a small 2D drawing library for building UIs on a framebuffer.
;;
;; The OS hands a display consumer a framebuffer (a `bytes` of 32-bit pixels, a byte
;; stride/pitch, and pixel dimensions -- see coredisplay / lfb / virtio-gpu). This
;; library wraps that in a `surface` and draws into it: solid rectangles, outlines,
;; lines, circles, image blits (opaque + alpha), and -- via font.clp -- text. It is
;; PURE: it only reads/writes the framebuffer bytes (the gfx-*! bulk primitives are
;; ambient data ops, like bytes-fill32!), so it needs NO capability and is fully
;; host-testable by drawing into a plain make-bytes buffer and asserting pixels.
;;
;; THE HW-2D OVERRIDE SEAM. A surface carries a `backend`: an alist of
;; (op-symbol . procedure) overrides. Every drawing op first looks itself up in the
;; backend; if a driver supplied an accelerated implementation (e.g. a GPU fill/blit
;; issued over its command queue), that runs, otherwise the software fallback (the
;; gfx-*! primitives) writes the shared framebuffer directly. So a plain linear
;; framebuffer uses all-software drawing, while a display driver that supports
;; hardware 2D can hand out a surface whose backend accelerates fill-rect/blit/...
;; and still fall back to software for any op it does not implement. Backend ops
;; take the SAME arguments as the software op they replace.
;;
;; Pixels are 32-bit; the surface stores the framebuffer's R/G/B channel bit offsets
;; (default 16/8/0 = 0x00RRGGBB) so `rgb` packs colours correctly for that layout.
;; Clipping is to the surface bounds (the gfx-*! primitives clip every target rect),
;; so off-screen / partially-off-screen draws are always safe; a sub-rectangle
;; scissor is left for a future revision.
(define-module graphics
  (export make-surface make-surface*
          surface-fb surface-width surface-height surface-stride
          surface-r-off surface-g-off surface-b-off surface-backend
          rgb argb
          clear fill-rect draw-rect draw-hline draw-vline draw-line
          put-pixel get-pixel draw-circle fill-circle
          blit blit-alpha draw-glyph
          make-double-buffer db-back db-front db-flush db-flush-rect
          make-damage damage-add! damage-rects damage-clear! damage-empty?
          db-flush-damage)
  (import driver-util)

  ;; --- the surface record -----------------------------------------------------
  ;; (fb width height stride r-off g-off b-off backend). backend is an alist of
  ;; (op . proc) HW overrides, or '() for all-software.
  (define (make-surface* fb width height stride r-off g-off b-off backend)
    (list fb width height stride r-off g-off b-off backend))
  ;; The common case: a software surface over a 0xRRGGBB framebuffer.
  (define (make-surface fb width height stride)
    (make-surface* fb width height stride 16 8 0 '()))

  (define (surface-fb s)      (nth s 0))
  (define (surface-width s)   (nth s 1))
  (define (surface-height s)  (nth s 2))
  (define (surface-stride s)  (nth s 3))
  (define (surface-r-off s)   (nth s 4))
  (define (surface-g-off s)   (nth s 5))
  (define (surface-b-off s)   (nth s 6))
  (define (surface-backend s) (nth s 7))

  ;; Look up an op override in the surface backend (a (symbol . proc) alist); #f if
  ;; the backend doesn't accelerate this op (-> software fallback).
  (define (backend-op s op)
    (let loop ((b (surface-backend s)))
      (cond ((null? b) #f)
            ((eq? (caar b) op) (cdar b))
            (else (loop (cdr b))))))

  ;; --- colour packing ---------------------------------------------------------
  ;; rgb: an opaque pixel in the surface's channel layout. argb: the same plus an
  ;; 8-bit alpha in the top byte, for blit-alpha source pixels.
  (define (rgb s r g b)
    (bitwise-or (arithmetic-shift (bitwise-and r 255) (surface-r-off s))
                (arithmetic-shift (bitwise-and g 255) (surface-g-off s))
                (arithmetic-shift (bitwise-and b 255) (surface-b-off s))))
  (define (argb s a r g b)
    (bitwise-or (arithmetic-shift (bitwise-and a 255) 24) (rgb s r g b)))

  ;; --- solid fills + rectangles -----------------------------------------------
  (define (fill-rect s x y w h color)
    (let ((op (backend-op s 'fill-rect)))
      (if op (op s x y w h color)
          (gfx-fill-rect! (surface-fb s) (surface-stride s)
                          (surface-width s) (surface-height s) x y w h color))))

  (define (clear s color)
    (fill-rect s 0 0 (surface-width s) (surface-height s) color))

  ;; A 1px-thick horizontal / vertical line is just a 1-tall / 1-wide fill.
  (define (draw-hline s x y w color) (fill-rect s x y w 1 color))
  (define (draw-vline s x y h color) (fill-rect s x y 1 h color))

  ;; Rectangle outline of thickness `t` (4 fills: top, bottom, left, right).
  (define (draw-rect s x y w h t color)
    (fill-rect s x y w t color)                       ; top
    (fill-rect s x (- (+ y h) t) w t color)           ; bottom
    (fill-rect s x y t h color)                       ; left
    (fill-rect s (- (+ x w) t) y t h color))          ; right

  ;; --- per-pixel + sparse primitives (lines, circle outlines) -----------------
  ;; These touch O(line-length)/O(radius) pixels, not the O(area) per-pixel trap, so
  ;; an interpreted loop over a clipped put-pixel is fine.
  (define (put-pixel s x y color)
    (if (and (>= x 0) (< x (surface-width s)) (>= y 0) (< y (surface-height s)))
        (bytes-u32-set! (surface-fb s) (+ (* y (surface-stride s)) (* x 4)) color)))

  (define (get-pixel s x y)
    (if (and (>= x 0) (< x (surface-width s)) (>= y 0) (< y (surface-height s)))
        (bytes-u32-ref (surface-fb s) (+ (* y (surface-stride s)) (* x 4)))
        0))

  (define (iabs n) (if (< n 0) (- n) n))

  ;; Bresenham line.
  (define (draw-line s x0 y0 x1 y1 color)
    (let ((dx (iabs (- x1 x0))) (dy (iabs (- y1 y0)))
          (sx (if (< x0 x1) 1 -1)) (sy (if (< y0 y1) 1 -1)))
      (let loop ((x x0) (y y0) (err (- dx dy)))
        (put-pixel s x y color)
        (if (and (= x x1) (= y y1))
            'done
            (let ((e2 (* 2 err)))
              (let ((nx (if (> e2 (- dy)) (+ x sx) x))
                    (ne (cond ((and (> e2 (- dy)) (< e2 dx)) (+ (- err dy) dx))
                              ((> e2 (- dy)) (- err dy))
                              ((< e2 dx) (+ err dx))
                              (else err)))
                    (ny (if (< e2 dx) (+ y sy) y)))
                (loop nx ny ne)))))))

  ;; Midpoint circle outline (8-way symmetry).
  (define (draw-circle s cx cy r color)
    (let loop ((x 0) (y r) (d (- 1 r)))
      (if (> x y)
          'done
          (begin
            (put-pixel s (+ cx x) (+ cy y) color) (put-pixel s (- cx x) (+ cy y) color)
            (put-pixel s (+ cx x) (- cy y) color) (put-pixel s (- cx x) (- cy y) color)
            (put-pixel s (+ cx y) (+ cy x) color) (put-pixel s (- cx y) (+ cy x) color)
            (put-pixel s (+ cx y) (- cy x) color) (put-pixel s (- cx y) (- cy x) color)
            (if (< d 0)
                (loop (+ x 1) y (+ d (+ (* 2 x) 3)))
                (loop (+ x 1) (- y 1) (+ d (+ (* 2 (- x y)) 5))))))))

  ;; Filled circle: one horizontal fill per scanline (each span is a fast fill-rect,
  ;; so this is O(radius) primitive calls, not O(area) per-pixel).
  (define (fill-circle s cx cy r color)
    (let loop ((dy (- r)))
      (if (> dy r)
          'done
          (let ((hw (isqrt (- (* r r) (* dy dy)))))
            (fill-rect s (- cx hw) (+ cy dy) (+ (* 2 hw) 1) 1 color)
            (loop (+ dy 1))))))

  ;; Integer sqrt (floor), for the circle span half-width.
  (define (isqrt n)
    (if (<= n 0) 0
        (let loop ((x n) (y (quotient (+ n 1) 2)))
          (if (< y x) (loop y (quotient (+ y (quotient n y)) 2)) x))))

  ;; --- image blits ------------------------------------------------------------
  ;; src is itself a surface (its fb/stride/dimensions describe the image). blit is
  ;; opaque; blit-alpha composites src (ARGB, alpha in the top byte) over the dst.
  (define (blit dst src dx dy)
    (let ((op (backend-op dst 'blit)))
      (if op (op dst src dx dy)
          (gfx-blit! (surface-fb dst) (surface-stride dst) (surface-width dst) (surface-height dst)
                     dx dy (surface-fb src) (surface-stride src)
                     (surface-width src) (surface-height src)))))

  (define (blit-alpha dst src dx dy)
    (let ((op (backend-op dst 'blit-alpha)))
      (if op (op dst src dx dy)
          (gfx-blend! (surface-fb dst) (surface-stride dst) (surface-width dst) (surface-height dst)
                      dx dy (surface-fb src) (surface-stride src)
                      (surface-width src) (surface-height src)))))

  ;; --- glyph blit (the primitive font.clp builds text on) ---------------------
  ;; Blit a 1-bpp glyph from `bitmap` at byte offset `boff` (gw*gh pixels, row
  ;; stride ceil(gw/8) bytes, MSB-left) at (x,y), each glyph pixel scaled to a
  ;; scale*scale block. Set bits paint `fg`; clear bits paint `bg` only when
  ;; `draw-bg?` is true (else transparent).
  (define (draw-glyph s x y bitmap boff gw gh fg bg draw-bg? scale)
    (let ((op (backend-op s 'draw-glyph)))
      (if op (op s x y bitmap boff gw gh fg bg draw-bg? scale)
          (gfx-glyph! (surface-fb s) (surface-stride s) (surface-width s) (surface-height s)
                      x y bitmap boff gw gh fg bg (if draw-bg? 1 0) scale))))

  ;; --- double buffering -------------------------------------------------------
  ;; A linear framebuffer is best mapped write-combining (mmio-map-wc): the CPU
  ;; coalesces sequential stores into bursts. But WC reads are slow and stores are
  ;; weakly ordered, so it is a poor surface to COMPOSE on (every blend/glyph op
  ;; reads back the destination). The fix is a double buffer: draw into a normal
  ;; cached (WB) back-buffer at full CPU/cache bandwidth, then stream the finished
  ;; frame to the WC front in one bulk copy.
  ;;
  ;; `front` is the scanout surface (ideally WC-mapped). make-double-buffer
  ;; allocates a cached back-buffer of identical geometry; draw on (db-back db),
  ;; then (db-flush db) (whole frame) or (db-flush-rect db x y w h) (one dirty
  ;; region). Channel offsets are inherited so colours match the scanout.
  (define (make-double-buffer front)
    (let* ((h      (surface-height front))
           (stride (surface-stride front))
           (back   (make-surface* (make-bytes (* stride h))
                                  (surface-width front) h stride
                                  (surface-r-off front) (surface-g-off front)
                                  (surface-b-off front) '())))
      (cons back front)))

  (define (db-back db)  (car db))
  (define (db-front db) (cdr db))

  ;; Whole-frame flush: one bulk store stream into the (WC) front buffer, then a
  ;; fence so the write-combine buffer drains before the scanout reads VRAM (else
  ;; the frame's tail edge can tear).
  (define (db-flush db)
    (let ((b (surface-fb (car db))))
      (bytes-copy! (surface-fb (cdr db)) 0 b 0 (bytes-length b))
      (sfence)))

  ;; Dirty-rect flush: copy only rows [y, y+h) of the [x, x+w) span. Each row is a
  ;; contiguous run; clipped to the buffer bounds so an oversized rect is safe.
  ;; Relies on back and front sharing a stride (make-double-buffer guarantees it,
  ;; copying front's stride into the back); the row offset is computed once and
  ;; used for both. A trailing sfence drains the WC buffer (see db-flush).
  (define (db-flush-rect db x y w h)
    (let* ((back   (car db)) (front (cdr db))
           (sw     (surface-width back)) (sh (surface-height back))
           (stride (surface-stride back))
           (x0 (if (< x 0) 0 x)) (y0 (if (< y 0) 0 y))
           (x1 (let ((e (+ x w))) (if (> e sw) sw e)))
           (y1 (let ((e (+ y h))) (if (> e sh) sh e)))
           (bf (surface-fb back)) (ff (surface-fb front)))
      (if (and (< x0 x1) (< y0 y1))
          (let ((rowbytes (* (- x1 x0) 4)))
            (let loop ((row y0))
              (if (< row y1)
                  (let ((off (+ (* row stride) (* x0 4))))
                    (bytes-copy! ff off bf off rowbytes)
                    (loop (+ row 1)))
                  (sfence))))
          'done)))

  ;; --- damage tracking --------------------------------------------------------
  ;; A frame usually changes only a few small regions; redrawing/flushing the whole
  ;; surface wastes most of the work. The damage list is where a UI records which
  ;; rectangles it dirtied this frame; the consumer (db-flush-damage here, or the
  ;; virtio-gpu driver's flush-rects message) then touches only those. Damage SOURCE
  ;; is the caller's job -- a component knows what it changed -- which is cheaper and
  ;; more precise than diffing the framebuffer. Each rect is (x y w h); the list is a
  ;; mutable cell so accumulation across draw calls needs no threading.
  (define (make-damage) (make-cell '()))
  (define (damage-add! d x y w h) (cell-set! d (cons (list x y w h) (cell-ref d))))
  (define (damage-rects d) (cell-ref d))
  (define (damage-clear! d) (cell-set! d '()))
  (define (damage-empty? d) (null? (cell-ref d)))

  ;; Flush just the damaged rects of a double-buffer to its (WC) front, then clear
  ;; the list for the next frame. The per-rect copies share one trailing fence.
  (define (db-flush-damage db d)
    (for-each (lambda (r) (db-flush-rect db (nth r 0) (nth r 1) (nth r 2) (nth r 3)))
              (damage-rects d))
    (damage-clear! d)))
