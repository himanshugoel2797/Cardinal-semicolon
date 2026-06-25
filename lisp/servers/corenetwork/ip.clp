;; corenetwork/ip: IPv4 build + receive/demux.

;; Build an IPv4 packet (20-byte header + payload) for `protocol`. The header
;; checksum covers the 20-byte header with its own field zero.
(define (build-ipv4 ip dst-ip protocol payload plen)
  (let ((total (+ 20 plen)))
    (let ((b (make-bytes total)))
      (bytes-u8-set! b 0 #x45)        ; version 4, ihl 5
      (put-be16! b 2 total)           ; total length
      (bytes-u8-set! b 8 64)          ; ttl
      (bytes-u8-set! b 9 protocol)
      (put-list! b 12 ip)             ; src ip (network order = the 4 bytes)
      (put-list! b 16 dst-ip)         ; dst ip
      (put-be16! b 10 (csum b 0 20))  ; header checksum
      (bytes-copy-into! b 20 payload plen)
      b)))

;; Pseudo-header carry-in for the UDP checksum: src ip, dst ip, zero, proto,
;; UDP length -- summed as big-endian 16-bit words.
(define (udp-pseudo-sum ip dst-ip seg-len)
  (+ (get-be16-list ip 0) (get-be16-list ip 2)
     (get-be16-list dst-ip 0) (get-be16-list dst-ip 2)
     IP-UDP seg-len))

;; Received IPv4 (frame; IP header at offset 14). Validate, then dispatch ICMP
;; echo-request -> echo-reply and UDP -> bound handler. The egress for any reply
;; is the interface that owns the packet's dst-IP (the address the peer targeted);
;; a limited-broadcast packet (DHCP OFFER/ACK while still 0.0.0.0) has no owning
;; interface and is only meaningful for UDP delivery.
(define (handle-ip ifaces routes cache binds tcp frame len)
  (let ((o 14))                       ; IP header start
    (if (or (< len 34) (not (= (bit-extract (u8 frame o) 4 4) 4)))
        'ignore
        (let* ((ihl (* (bit-extract (u8 frame o) 0 4) 4))
               (proto (u8 frame (+ o 9)))
               (src-ip (read-ip frame (+ o 12)))
               (dst-ip (read-ip frame (+ o 16)))
               (dst-if (iface-for-ip ifaces dst-ip)))   ; the interface addressed, or #f
          ;; Drop unless: ihl is sane and the whole header lies within the bytes
          ;; we actually received (a lying ihl would otherwise drive csum past
          ;; the frame -> bytes-u8-ref error -> the service context dies), it is
          ;; addressed to us (one of our interface IPs OR the limited broadcast),
          ;; and the header checksum is valid (a good header, including its own
          ;; field, folds to 0). ihl/len guards precede the csum read (`or` short-
          ;; circuits). Replies use the addressed interface's ip/mac/tx.
          (if (or (< ihl 20) (> (+ o ihl) len)
                  (not (or dst-if (equal? dst-ip IP-BROADCAST)))
                  (not (= 0 (csum frame o ihl))))
              'ignore
              (let ((l4 (+ o ihl))        ; transport header offset
                    (ip (if dst-if (ig dst-if I-IP) IP-ANY))
                    (mac (if dst-if (ig dst-if I-MAC) #f))
                    (nic-tx (if dst-if (ig dst-if I-TX) #f)))
                (cond
                  ((and (= proto IP-ICMP) dst-if)
                   (handle-icmp ip mac nic-tx cache src-ip frame l4 len))
                  ((= proto IP-UDP)
                   (handle-udp binds src-ip dst-ip (read-mac frame 6) frame l4 len))
                  ;; TCP mutates the connection tables in place (no state to
                  ;; thread back); it replies on the captured src-mac. TCP carries
                  ;; no length field, so the segment end must come from the IP
                  ;; total-length field, NOT the frame len -- a min-size segment is
                  ;; padded to the 60-byte ethernet minimum, and summing that
                  ;; padding into the checksum would wrongly reject it.
                  ((and (= proto IP-TCP) dst-if)
                   (let ((iend (+ o (get-be16 frame (+ o 2)))))
                     ;; ip/mac/nic-tx are the addressed interface's: a connection
                     ;; opened here egresses on the link it arrived on.
                     (handle-tcp ip mac nic-tx tcp frame l4
                                 (if (> iend len) len iend) src-ip)))
                  (else 'ignore))))))))
