;; corenetwork/udp: UDP receive (dispatch to a bound handler) + send.

;; UDP at offset `l4`. If a handler is bound to the dest port, hand it the
;; payload; otherwise drop. Checksum 0 means "no checksum" (RFC 768).
(define (handle-udp binds src-ip dst-ip src-mac frame l4 len)
  (let ((ulen (- len l4)))
    (if (< ulen 8)
        'ignore
        (let ((dport (get-be16 frame (+ l4 2)))
              (sport (get-be16 frame (+ l4 0)))
              (seglen (get-be16 frame (+ l4 4)))
              (ucsum (get-be16 frame (+ l4 6))))
          ;; Clamp the summed length to what we actually received so a lying
          ;; length can't over-read; a too-SHORT length (< the 8-byte header)
          ;; is malformed and dropped -- otherwise plen would go negative and
          ;; (make-bytes <neg>) would error out and kill the service context
          ;; (the C's udp_seg_len rejected seg_len < sizeof(udp_t)). A non-zero
          ;; checksum must verify; 0 means "no checksum" (RFC 768).
          (let ((seg (if (> seglen ulen) ulen seglen)))
            (if (or (< seg 8)
                    (and (not (= ucsum 0))
                         (not (= 0 (csum-seeded frame l4 seg
                                                (udp-pseudo-sum src-ip dst-ip seglen))))))
                'ignore
                ;; Deliver to EVERY handler bound to dport, not just the first.
                ;; This is what lets several DHCP clients (one per interface, all on
                ;; port 68) coexist: each OFFER/ACK reaches all of them and each
                ;; accepts only its own (filtered by xid/MAC). For the common single-
                ;; binder port this is identical to a first-match delivery.
                (let ((payload (copy-bytes frame (+ l4 8) (- seg 8))))
                  (let loop ((bs binds) (n 0))
                    (cond ((null? bs) (if (> n 0) 'delivered 'ignore))
                          ((= (car (car bs)) dport)
                           (send (cdr (car bs)) (list 'udp-rx src-ip src-mac sport payload))
                           (loop (cdr bs) (+ n 1)))
                          (else (loop (cdr bs) n)))))))))))

;; Build the IP packet (header + UDP segment) for a datagram, returning the
;; bytes (length 20 + 8 + plen). A computed checksum of 0 is sent as 0xFFFF (0 on
;; the wire means "no checksum"); the pseudo-header + segment must sum to zero at
;; the receiver, so we seed the segment sum with the pseudo-header. Split out from
;; udp-send so the async TX path can build the frame and defer the ethernet send.
(define (udp-build-ip ip dst-ip sport dport payload plen)
  (let ((seg (+ 8 plen)))
    (let ((u (make-bytes seg)))
      (put-be16! u 0 sport)
      (put-be16! u 2 dport)
      (put-be16! u 4 seg)             ; UDP length
      (bytes-copy-into! u 8 payload plen)
      (let ((c (csum-seeded u 0 seg (udp-pseudo-sum ip dst-ip seg))))
        (put-be16! u 6 (if (= c 0) #xFFFF c)))
      (build-ipv4 ip dst-ip IP-UDP u seg))))   ; the whole IP packet (header + UDP)

(define (udp-send ip mac nic-tx dst-ip dst-mac sport dport payload plen)
  (eth-tx nic-tx mac dst-mac ETH-IPV4
          (udp-build-ip ip dst-ip sport dport payload plen) (+ 28 plen)))
