;; coredisplay: the display registry + EDID parser, ported from servers/CoreDisplay.
;;
;; The C server kept a list of display devices (a driver registers its handler
;; table) and, if none registered, loaded the lfb fallback. Here the registry is
;; the usual `serve` loop -- a display driver `send`s (register <name> <conn>
;; <ctx>). The substantive part is coredisplay_parse_edid (servers/.../edid.c),
;; a pure byte-buffer parser turning a 128-byte EDID blob into resolutions and a
;; monitor name; it ports directly to Lisp byte operations -- no capability, no
;; device access, just bit-twiddling -- so it lives here as `parse-edid`.
;;
;; parse-edid returns an alist:
;;   ((bit-depth . N) (gamma . N) (established . N)
;;    (standard-timings . ((h-res v-res v-freq aspect-num aspect-denom) ...))
;;    (display-name . "string")
;;    (detailed-modes . (<mode-alist> ...)))
;; or #f if the header is bad or a detailed mode is analog (unsupported, exactly
;; as the C returned false). A detailed mode alist carries the timing geometry
;; (pixel-clock, hactive/vactive, blanking, sync porch/pulse, physical size, ...).

(define-module coredisplay
  (export start-display-service parse-edid)
  (import driver-util)
  (define lg (make-logger 'coredisplay))

  ;; --- the display registry ---------------------------------------------------
  (define (start-display-service)
    (serve '()
      (lambda (disps m)
        (cond ((eq? (car m) 'register)        ; (register name connection ctx)
               (lg "display registered: " (cadr m))
               (cons (cdr m) disps))
              (else disps)))))

  ;; --- EDID parser (pure) -----------------------------------------------------
  (define (u8 b i) (bytes-u8-ref b i))

  ;; The 8-byte EDID magic: 00 FF FF FF FF FF FF 00.
  (define (edid-header-ok? b)
    (and (= (u8 b 0) 0)    (= (u8 b 1) #xFF) (= (u8 b 2) #xFF) (= (u8 b 3) #xFF)
         (= (u8 b 4) #xFF) (= (u8 b 5) #xFF) (= (u8 b 6) #xFF) (= (u8 b 7) 0)))

  ;; Byte 20: bit 7 = digital; bits 6:4 = colour bit depth code. Analog/legacy
  ;; (bit 7 clear) leaves the depth 0, matching the C path that skips the block; a
  ;; reserved code (7) on a digital input is a parse failure (#f), as in the C.
  (define (edid-bit-depth b)
    (if (= 0 (bit-extract (u8 b 20) 7 1))
        0
        (let ((code (bit-extract (u8 b 20) 4 3)))
          (cond ((= code 0) 0)  ((= code 1) 6)  ((= code 2) 8)  ((= code 3) 10)
                ((= code 4) 12) ((= code 5) 14) ((= code 6) 16) (else #f)))))

  ;; Established-timings bitmap, bytes 35-37 little-endian.
  (define (edid-established b)
    (bitwise-or (u8 b 35)
                (arithmetic-shift (u8 b 36) 8)
                (arithmetic-shift (u8 b 37) 16)))

  ;; The 8 standard-timing slots (bytes 38-53), 2 bytes each. An unused slot is
  ;; 0x01 0x01 and is skipped. h-res = (byte0 + 31) * 8; the aspect code in the
  ;; top 2 bits of byte1 gives the ratio and hence v-res; v-freq = low 6 bits + 60.
  (define (edid-standard b)
    (let loop ((i 0) (acc '()))
      (if (>= i 16)
          (reverse acc)
          (if (and (= (u8 b (+ 38 i)) 1) (= (u8 b (+ 39 i)) 1))
              (loop (+ i 2) acc)
              (let* ((hres (* (+ (u8 b (+ 38 i)) 31) 8))
                     (code (bit-extract (u8 b (+ 39 i)) 6 2))
                     (vfreq (+ (bit-extract (u8 b (+ 39 i)) 0 5) 60))
                     (asp (cond ((= code 0) (cons 16 10)) ((= code 1) (cons 4 3))
                                ((= code 2) (cons 5 4))  (else (cons 16 9))))
                     (vres (quotient (* hres (cdr asp)) (car asp))))
                (loop (+ i 2)
                      (cons (list hres vres vfreq (car asp) (cdr asp)) acc)))))))

  ;; A 0xFC display-descriptor's 13-byte ASCII name (bytes off..off+12), trimmed
  ;; at the 0x0A terminator EDID pads names with.
  (define (edid-name b off)
    (let loop ((i 0) (chars '()))
      (if (or (= i 13) (= (u8 b (+ off i)) #x0A))
          (list->string (reverse chars))
          (loop (+ i 1) (cons (integer->char (u8 b (+ off i))) chars)))))

  ;; One 18-byte detailed timing descriptor at base `o`: the spec's split-nibble
  ;; layout, assembled field by field (mirrors edid.c line for line).
  (define (edid-mode b o)
    (list
      (cons 'pixel-clock (+ (u8 b o) (arithmetic-shift (u8 b (+ o 1)) 8)))
      (cons 'hactive (+ (u8 b (+ o 2)) (arithmetic-shift (bit-extract (u8 b (+ o 4)) 4 4) 8)))
      (cons 'hblank  (+ (u8 b (+ o 3)) (arithmetic-shift (bit-extract (u8 b (+ o 4)) 0 4) 8)))
      (cons 'vactive (+ (u8 b (+ o 5)) (arithmetic-shift (bit-extract (u8 b (+ o 7)) 4 4) 8)))
      (cons 'vblank  (+ (u8 b (+ o 6)) (arithmetic-shift (bit-extract (u8 b (+ o 7)) 0 4) 8)))
      (cons 'hsync-porch (+ (u8 b (+ o 8)) (arithmetic-shift (bit-extract (u8 b (+ o 11)) 6 2) 8)))
      (cons 'hsync-pulse (+ (u8 b (+ o 9)) (arithmetic-shift (bit-extract (u8 b (+ o 11)) 4 2) 8)))
      (cons 'vsync-porch (+ (bit-extract (u8 b (+ o 10)) 4 4)
                            (arithmetic-shift (bit-extract (u8 b (+ o 11)) 2 2) 4)))
      (cons 'vsync-pulse (+ (bit-extract (u8 b (+ o 10)) 0 4)
                            (arithmetic-shift (bit-extract (u8 b (+ o 11)) 0 2) 4)))
      (cons 'hsize-mm (+ (u8 b (+ o 12)) (arithmetic-shift (bit-extract (u8 b (+ o 14)) 4 4) 8)))
      (cons 'vsize-mm (+ (u8 b (+ o 13)) (arithmetic-shift (bit-extract (u8 b (+ o 14)) 0 4) 8)))
      (cons 'hborder (u8 b (+ o 15)))
      (cons 'vborder (u8 b (+ o 16)))))

  ;; The four 18-byte descriptors at byte 54. A descriptor whose first two bytes
  ;; are 0 is a display descriptor (we extract the 0xFC name); otherwise it is a
  ;; timing descriptor -- but only digital (byte+17 bit 4 set); an analog mode
  ;; fails the whole parse (#f), as the C did. Returns (modes . name).
  (define (edid-detailed b)
    (let loop ((i 0) (modes '()) (name ""))
      (if (>= i 72)
          (cons (reverse modes) name)
          (let ((o (+ 54 i)))
            (if (and (= (u8 b o) 0) (= (u8 b (+ o 1)) 0))
                (if (= (u8 b (+ o 3)) #xFC)
                    (loop (+ i 18) modes (edid-name b (+ o 5)))
                    (loop (+ i 18) modes name))
                (if (= 0 (bit-extract (u8 b (+ o 17)) 4 1))
                    #f                                  ; analog: unsupported
                    (loop (+ i 18) (cons (edid-mode b o) modes) name)))))))

  (define (parse-edid b)
    (if (not (edid-header-ok? b))
        #f
        (let ((bd (edid-bit-depth b)))
          (if (not bd)
              #f
              (let ((det (edid-detailed b)))
                (if (not det)
                    #f
                    (list (cons 'bit-depth bd)
                          (cons 'gamma (u8 b 23))
                          (cons 'established (edid-established b))
                          (cons 'standard-timings (edid-standard b))
                          (cons 'display-name (cdr det))
                          (cons 'detailed-modes (car det))))))))))
