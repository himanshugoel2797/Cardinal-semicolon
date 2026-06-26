;; ttf: antialiased TrueType text rendering, layered on graphics.clp.
;;
;; A `ttf-font` wraps the raw TTF bytes plus two memo caches (a glyph cache and a
;; vertical-metrics cache), both equal?-keyed hash tables. Rendering a glyph the
;; first time calls the kernel `ttf-rasterize` primitive (libs/ttf / stb_truetype),
;; which returns an 8-bit coverage bitmap + pen metrics; thereafter it is a cache
;; hit. Drawing composites that coverage as a solid colour over the surface with the
;; ambient `gfx-cover!` primitive (antialiased blend), so text inherits the same
;; clipping + (eventually) hardware-accelerated path the rest of graphics.clp does.
;;
;; Positioning follows stb's pen model: text is laid out from a top-left (x,y); the
;; baseline sits `ascent` pixels below y, and each glyph's coverage box is placed at
;; (x + xoff, baseline + yoff) (yoff is negative -- the box top is above the
;; baseline). The pen advances by the glyph's horizontal advance. Newlines drop one
;; line pitch (ascent - descent + linegap) and return to the start column.
;;
;; The font bytes are loaded by a context holding sys-initrd (init reads
;; TTF-FONT-PATH and calls make-ttf-font); this module needs only sys-ttf + graphics.
(define-module ttf
  (export make-ttf-font ttf-draw-text ttf-text-width ttf-line-height
          ttf-ascent TTF-FONT-PATH)
  (import sys-ttf graphics driver-util)

  (define TTF-FONT-PATH "./lisp/data/DejaVuSans-subset.ttf")
  (define NEWLINE (integer->char 10))   ; the reader has no #\newline

  ;; A font: (bytes glyph-cache vmetrics-cache).
  (define (make-ttf-font bytes) (list bytes (make-hash-table) (make-hash-table)))
  (define (ttf-bytes f)    (nth f 0))
  (define (ttf-gcache f)   (nth f 1))
  (define (ttf-vcache f)   (nth f 2))

  ;; A glyph record is exactly ttf-rasterize's result: (coverage w h xoff yoff adv),
  ;; where coverage is a w*h 8-bit alpha bitmap, or #f for an empty glyph (e.g. space).
  (define (g-cov g) (nth g 0))   (define (g-w g)    (nth g 1))
  (define (g-h g)   (nth g 2))   (define (g-xoff g) (nth g 3))
  (define (g-yoff g) (nth g 4))  (define (g-adv g)  (nth g 5))

  ;; Rasterize (codepoint,px) once, then serve from the glyph cache. The key folds
  ;; both into one fixnum (px is small; the font is Latin), so no allocation per key.
  (define (glyph font cp px)
    (let ((cache (ttf-gcache font)) (key (+ (* cp 100000) px)))
      (if (hash-has-key? cache key)
          (hash-ref cache key)
          (let ((g (ttf-rasterize (ttf-bytes font) cp px)))
            (hash-set! cache key g)
            g))))

  ;; Cached vertical metrics (ascent descent linegap) at `px`.
  (define (vmetrics font px)
    (let ((cache (ttf-vcache font)))
      (if (hash-has-key? cache px)
          (hash-ref cache px)
          (let ((vm (ttf-vmetrics (ttf-bytes font) px))) (hash-set! cache px vm) vm))))

  (define (ttf-ascent font px) (car (vmetrics font px)))

  ;; Line pitch: ascent - descent + linegap (descent is negative).
  (define (ttf-line-height font px)
    (let ((vm (vmetrics font px)))
      (+ (- (car vm) (cadr vm)) (caddr vm))))

  ;; Draw `str` with its top-left at (x,y), in `color`, at `px` pixel height (color
  ;; before size, matching the bitmap draw-text). Glyphs whose coverage box falls off
  ;; the surface are clipped by gfx-cover!. Newlines wrap to column x and the next
  ;; line. Returns the baseline y of the last line.
  (define (ttf-draw-text surf font x y str color px)
    (let ((fb (surface-fb surf)) (st (surface-stride surf))
          (sw (surface-width surf)) (sh (surface-height surf))
          (lh (ttf-line-height font px)) (n (string-length str)))
      (let loop ((i 0) (cx x) (base (+ y (ttf-ascent font px))))
        (if (>= i n)
            base
            (let ((ch (string-ref str i)))
              (if (char=? ch NEWLINE)
                  (loop (+ i 1) x (+ base lh))
                  (let* ((g (glyph font (char->integer ch) px)) (cov (g-cov g)))
                    (if cov
                        (gfx-cover! fb st sw sh
                                    (+ cx (g-xoff g)) (+ base (g-yoff g))
                                    cov (g-w g) (g-w g) (g-h g) color))
                    (loop (+ i 1) (+ cx (g-adv g)) base))))))))

  ;; Pixel width of `str` at `px` (widest line for multi-line text), summing the
  ;; cached glyph advances.
  (define (ttf-text-width font str px)
    (let ((n (string-length str)))
      (let loop ((i 0) (cur 0) (best 0))
        (if (>= i n)
            (if (> cur best) cur best)
            (let ((ch (string-ref str i)))
              (if (char=? ch NEWLINE)
                  (loop (+ i 1) 0 (if (> cur best) cur best))
                  (loop (+ i 1) (+ cur (g-adv (glyph font (char->integer ch) px))) best))))))))
