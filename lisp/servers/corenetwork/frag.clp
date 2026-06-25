;; corenetwork/frag: IPv4 fragmentation (outbound) + reassembly (inbound).
;;
;; Outbound: eth-tx-ip sends an IP packet, splitting it into fragments when it
;; exceeds the link MTU (all fragments share one IP id; non-last fragments carry
;; the MF flag and a /8 fragment offset). TCP is MSS-bounded so never fragments;
;; this matters for large UDP/ICMP.
;;
;; Inbound: reasm-offer buffers a received fragment keyed by (src dst proto id),
;; and once the fragments cover [0,total) contiguously it SYNTHESIZES a single
;; unfragmented frame (ethernet + IP header + reassembled payload) and returns it,
;; which handle-ip re-dispatches as a normal packet. Incomplete datagrams are
;; dropped after a timeout. Bounded (entry count + per-datagram size) against a
;; fragment-flood DoS.

(define IP-MTU 1500)                 ; max IP packet (ethernet payload) in bytes
(define REASM-MAX 8)                 ; concurrent in-progress reassemblies
(define REASM-MAX-LEN 65535)         ; max reassembled payload (IP total-length cap)
(define REASM-DROP-NS 5000000000)    ; drop an incomplete datagram after 5s

;; --- outbound fragmentation -------------------------------------------------
;; A module-private 16-bit IP identification counter shared by all interfaces
;; (the id only needs to distinguish a sender's in-flight datagrams).
(define frag-id-ctr (make-cell 1))
(define (frag-next-id)
  (let ((id (cell-ref frag-id-ctr)))
    (cell-set! frag-id-ctr (bitwise-and (+ id 1) #xFFFF)) id))

;; Build one fragment: copy the `ihl`-byte header from `ip-pkt`, set this
;; fragment's id / flags+offset / total-length, recompute the header checksum, and
;; append the payload slice [poff, poff+clen). `more` sets the MF flag.
(define (frag-build ip-pkt ihl id poff clen more)
  (let ((f (make-bytes (+ ihl clen))))
    (bytes-copy-into! f 0 ip-pkt ihl)               ; copy IP header (+ options)
    (put-be16! f 2 (+ ihl clen))                    ; total length
    (put-be16! f 4 id)                              ; identification (shared)
    (put-be16! f 6 (bitwise-or (quotient poff 8) (if more #x2000 0)))  ; flags + frag offset
    (put-be16! f 10 0)                              ; zero checksum before recompute
    (put-be16! f 10 (csum f 0 ihl))
    (bytes-copy-into! f ihl (copy-bytes ip-pkt (+ ihl poff) clen) clen)
    f))

;; Send an IP packet as ethernet, fragmenting if it exceeds the MTU.
(define (eth-tx-ip nic-tx src-mac dst-mac ip-pkt iplen)
  (if (<= iplen IP-MTU)
      (eth-tx nic-tx src-mac dst-mac ETH-IPV4 ip-pkt iplen)
      (let* ((ihl (* (bit-extract (u8 ip-pkt 0) 0 4) 4))
             (paylen (- iplen ihl))
             (maxchunk (bitwise-and (- IP-MTU ihl) (bitwise-not 7)))  ; /8-aligned
             (id (frag-next-id)))
        (let loop ((poff 0))
          (if (< poff paylen)
              (let* ((rem (- paylen poff))
                     (clen (if (> rem maxchunk) maxchunk rem)))
                (eth-tx nic-tx src-mac dst-mac ETH-IPV4
                        (frag-build ip-pkt ihl id poff clen (> rem clen)) (+ ihl clen))
                (loop (+ poff clen))))))))

;; --- inbound reassembly -----------------------------------------------------
;; A reassembly buffer is a mutable cell holding a list of entries:
;;   (key frags total deadline)
;; key = (src-ip dst-ip proto id); frags = a list of (offset . data) kept sorted by
;; offset; total = the reassembled payload length once the last fragment (MF=0) is
;; seen, else #f; deadline = uptime-ns drop time.
(define (mk-reasm) (make-cell '()))
(define (re-key e) (nth e 0))
(define (re-frags e) (nth e 1))
(define (re-total e) (nth e 2))
(define (re-deadline e) (nth e 3))

(define (re-find es key)
  (cond ((null? es) #f)
        ((equal? (re-key (car es)) key) (car es))
        (else (re-find (cdr es) key))))

;; Insert (offset . data) into a by-offset-sorted frag list, dropping an exact
;; duplicate offset (a retransmitted fragment).
(define (frag-insert frags off data)
  (cond ((null? frags) (list (cons off data)))
        ((= (car (car frags)) off) frags)                 ; duplicate: keep existing
        ((< off (car (car frags))) (cons (cons off data) frags))
        (else (cons (car frags) (frag-insert (cdr frags) off data)))))

;; Are the sorted frags contiguous over [0,total)? If so, concatenate them into the
;; full payload bytes; else #f.
(define (frag-assemble frags total)
  (if (not total)
      #f
      (let ((out (make-bytes total)))
        (let loop ((fs frags) (expected 0))
          (cond ((= expected total) (if (null? fs) out #f))   ; exact cover, nothing extra
                ((null? fs) #f)                               ; gap before total
                ((= (car (car fs)) expected)
                 (let ((d (cdr (car fs))))
                   (bytes-copy-into! out expected d (bytes-length d))  ; place at its offset
                   (loop (cdr fs) (+ expected (bytes-length d)))))
                (else #f))))))                                ; gap / overlap

;; Offer a received fragment (frame; IP header at offset o, length ihl). Returns a
;; synthesized complete frame (bytes) if this fragment finishes the datagram, else
;; #f. Bounded so a flood of distinct ids cannot grow the buffer without end.
(define (reasm-offer reasm frame o ihl now)
  (let* ((iptot (get-be16 frame (+ o 2)))
         (ff (get-be16 frame (+ o 6)))
         (mf (> (bitwise-and ff #x2000) 0))
         (foff (* (bitwise-and ff #x1FFF) 8))
         (dlen (- iptot ihl))
         (key (list (read-ip frame (+ o 12)) (read-ip frame (+ o 16))
                    (u8 frame (+ o 9)) (get-be16 frame (+ o 4)))))
    (if (or (< dlen 0) (> (+ foff dlen) REASM-MAX-LEN))
        #f                                               ; nonsense offsets: drop
        (let* ((es (cell-ref reasm))
               (e (re-find es key))
               (data (copy-bytes frame (+ o ihl) dlen))
               (frags (frag-insert (if e (re-frags e) '()) foff data))
               (total (if mf (if e (re-total e) #f) (+ foff dlen)))  ; MF=0 fixes the total
               (e2 (list key frags total (+ now REASM-DROP-NS)))
               (rest (filter (lambda (x) (not (equal? (re-key x) key))) es)))
          (let ((full (frag-assemble frags total)))
            (if full
                (begin (cell-set! reasm rest)            ; done: remove the entry
                       (frag-synth-frame frame o ihl full))
                (begin                                   ; incomplete: keep it (bounded)
                  (cell-set! reasm (cons e2 (if (>= (length rest) REASM-MAX)
                                                (cdr rest) rest)))  ; evict oldest if full
                  #f)))))))

;; Build a complete unfragmented frame from a reassembled payload: the original
;; ethernet + IP header, with the fragment fields cleared, the total length set,
;; and the header checksum recomputed.
(define (frag-synth-frame frame o ihl payload)
  (let* ((plen (bytes-length payload))
         (n (+ o ihl plen))
         (f (make-bytes n)))
    (bytes-copy-into! f 0 (copy-bytes frame 0 (+ o ihl)) (+ o ihl))  ; eth + IP header
    (put-be16! f (+ o 2) (+ ihl plen))                  ; total length
    (put-be16! f (+ o 6) 0)                             ; clear flags + frag offset
    (put-be16! f (+ o 10) 0)
    (put-be16! f (+ o 10) (csum f o ihl))               ; recompute header checksum
    (bytes-copy-into! f (+ o ihl) payload plen)
    f))

;; Drop reassembly entries whose deadline has passed (the datagram never completed).
(define (reasm-tick! reasm now)
  (cell-set! reasm (filter (lambda (e) (<= now (re-deadline e))) (cell-ref reasm))))
