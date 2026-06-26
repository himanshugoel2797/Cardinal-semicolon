;; font: bitmap-font text rendering on top of graphics.clp's glyph blitter.
;;
;; A `font` is a fixed-cell bitmap font: a `bytes` glyph plane where glyph N (the
;; character code) occupies a gh-row, ceil(gw/8)-byte-per-row cell at byte offset
;; N*cellbytes, MSB = leftmost pixel. The default is the 8x16 VGA-style font shipped
;; as ./lisp/data/font8x16.bin (see lisp/data/README.md): 256 glyphs * 16 bytes,
;; ASCII 32..126 populated. `make-font` wraps any such plane; the loader that reads
;; the initrd lives in the consumer (init), which holds the sys-initrd capability --
;; this module stays PURE (render only), so it needs no capability and is fully
;; host-testable by handing make-font a buffer built in the test.
;;
;; Rendering routes through graphics' draw-glyph, so it inherits both the fast C
;; glyph blitter AND any hardware-accelerated glyph backend a display driver installs.
(define-module font
  (export make-font font-glyph-w font-glyph-h font-cellbytes
          draw-char draw-text text-width FONT8X16-PATH FONT8X16-W FONT8X16-H)
  (import graphics driver-util)

  ;; The default font's initrd path + cell size (the consumer reads the bytes with
  ;; sys-initrd and calls (make-font bytes FONT8X16-W FONT8X16-H)).
  (define FONT8X16-PATH "./lisp/data/font8x16.bin")
  (define FONT8X16-W 8)
  (define FONT8X16-H 16)

  ;; The reader supports only single-character #\x literals (named chars like
  ;; #\newline are not parsed), so spell newline as its code point.
  (define NEWLINE (integer->char 10))

  ;; A font record: (bitmap glyph-w glyph-h cellbytes count). cellbytes is the
  ;; per-glyph byte span (gh rows * ceil(gw/8) bytes), so glyph N starts at
  ;; N*cellbytes; count is how many glyphs the bitmap holds.
  (define (make-font bitmap gw gh)
    (let ((cb (* gh (quotient (+ gw 7) 8))))
      (list bitmap gw gh cb (quotient (bytes-length bitmap) cb))))
  (define (font-bitmap s)    (nth s 0))
  (define (font-glyph-w s)   (nth s 1))
  (define (font-glyph-h s)   (nth s 2))
  (define (font-cellbytes s) (nth s 3))
  (define (font-count s)     (nth s 4))

  ;; Draw one character's glyph at (x,y), scaled scale*scale. fg/bg are packed
  ;; pixels; when draw-bg? is false the background is transparent. A character code
  ;; outside the font's glyph range is skipped (drawn as blank), so a stray Unicode
  ;; char never pushes the glyph offset past the bitmap.
  (define (draw-char surf font x y ch fg bg draw-bg? scale)
    (let ((code (char->integer ch)))
      (if (and (>= code 0) (< code (font-count font)))
          (draw-glyph surf x y (font-bitmap font)
                      (* code (font-cellbytes font))
                      (font-glyph-w font) (font-glyph-h font) fg bg draw-bg? scale))))

  ;; Draw a string starting at (x,y), advancing one cell per character; a newline
  ;; returns to the start column and drops one (scaled) line. Returns the next y.
  (define (draw-text surf font x y str fg bg draw-bg? scale)
    (let ((adv (* (font-glyph-w font) scale))
          (lh  (* (font-glyph-h font) scale))
          (n   (string-length str)))
      (let loop ((i 0) (cx x) (cy y))
        (if (>= i n)
            cy
            (let ((ch (string-ref str i)))
              (if (char=? ch NEWLINE)
                  (loop (+ i 1) x (+ cy lh))
                  (begin (draw-char surf font cx cy ch fg bg draw-bg? scale)
                         (loop (+ i 1) (+ cx adv) cy))))))))

  ;; Pixel width of `str` at `scale` (longest line for multi-line text).
  (define (text-width font str scale)
    (let ((adv (* (font-glyph-w font) scale)) (n (string-length str)))
      (let loop ((i 0) (cur 0) (best 0))
        (if (>= i n)
            (* (if (> cur best) cur best) adv)
            (if (char=? (string-ref str i) NEWLINE)
                (loop (+ i 1) 0 (if (> cur best) cur best))
                (loop (+ i 1) (+ cur 1) best)))))))
