;; corenetwork/checksum: the internet checksum (RFC 1071), over big-endian 16-bit
;; words -- byte-identical on the wire to the C's native-order version.

(define (csum-fold sum)
  (let loop ((s sum))
    (if (> (arithmetic-shift s -16) 0)
        (loop (+ (bitwise-and s #xFFFF) (arithmetic-shift s -16)))
        (bitwise-and (bitwise-not s) #xFFFF))))

;; One's-complement sum over [off, off+len) plus a carry-in `seed` (for a UDP
;; pseudo-header). Folds and complements; a valid structure re-sums to 0.
(define (csum-seeded b off len seed)
  (let loop ((i off) (rem len) (sum seed))
    (cond ((> rem 1) (loop (+ i 2) (- rem 2) (+ sum (get-be16 b i))))
          ((= rem 1) (csum-fold (+ sum (arithmetic-shift (u8 b i) 8))))
          (else (csum-fold sum)))))

(define (csum b off len) (csum-seeded b off len 0))
