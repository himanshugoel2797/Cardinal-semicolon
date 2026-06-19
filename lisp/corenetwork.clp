;; corenetwork: the IPv4 network stack, ported from servers/CoreNetwork
;; (ethernet/arp/ip/icmp/udp). One long-lived service context owns the interface
;; (our IP + MAC), the ARP cache, and the UDP port table; it receives raw frames
;; from a NIC driver and demuxes ethernet -> ARP / IPv4 -> ICMP / UDP, and builds
;; replies it hands back to the NIC for transmission.
;;
;; The C server's synchronous call chain (ethernet_rx -> arp_rx -> ethernet_tx,
;; and the udp handler calling back into udp_send_to) is exactly the rx-handler-
;; re-enters-tx pattern that forced the "copy the handler out from under the lock"
;; dance there. Here every step is a message: the NIC sends (rx frame len), the
;; service sends the NIC (tx frame len), a UDP handler is a separate context the
;; service sends datagrams to -- no re-entrancy, no locks.
;;
;; Wire formats are big-endian; put-be16!/get-be16 (driver-util) do the byte
;; assembly. The internet checksum (RFC 1071) is computed over big-endian 16-bit
;; words -- byte-for-byte identical on the wire to the C's native-order version
;; (the one's-complement sum commutes with the per-word byte swap).
;;
;; Service protocol (send these to the handle from start-network-service):
;;   (register-nic <mac-list> <tx-ctx>)        ; the NIC announces itself
;;   (rx <frame-bytes> <len>)                  ; a received ethernet frame
;;   (arp-request <ip-list>)                   ; emit an ARP who-has
;;   (arp-lookup <ip-list> <reply-ctx>)        ; reply (mac | #f) from the cache
;;   (udp-bind <port> <handler-ctx>)           ; datagrams to <port> -> handler
;;   (udp-send <dst-ip> <dst-mac> <sport> <dport> <payload-bytes>)
;;   (ping <dst-ip> <dst-mac> <id> <seq>)      ; emit an ICMP echo request
;; A bound UDP handler receives (udp-rx <src-ip> <src-mac> <src-port> <payload>).

(define-module corenetwork
  (export start-network-service)
  (import driver-util)

  (define (u8 b i) (bytes-u8-ref b i))
  (define ETH-ARP  #x0806)
  (define ETH-IPV4 #x0800)
  (define IP-ICMP 1)
  (define IP-UDP 17)

  ;; --- small reads ------------------------------------------------------------
  (define (read-mac b off)
    (list (u8 b off) (u8 b (+ off 1)) (u8 b (+ off 2))
          (u8 b (+ off 3)) (u8 b (+ off 4)) (u8 b (+ off 5))))
  (define (read-ip b off)
    (list (u8 b off) (u8 b (+ off 1)) (u8 b (+ off 2)) (u8 b (+ off 3))))
  (define BROADCAST (list #xFF #xFF #xFF #xFF #xFF #xFF))
  ;; Pack a 4-byte IP list into a fixnum so it is an eq?-comparable cache key.
  (define (ip->key ip)
    (+ (arithmetic-shift (car ip) 24) (arithmetic-shift (cadr ip) 16)
       (arithmetic-shift (caddr ip) 8) (cadddr ip)))

  ;; --- internet checksum (RFC 1071), big-endian words -------------------------
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

  ;; --- ethernet TX ------------------------------------------------------------
  ;; Build an ethernet frame around `payload` (plen bytes) and send it to the NIC
  ;; for transmission. Frames shorter than 60 bytes are zero-padded (make-bytes
  ;; zero-fills), so a NIC that does not pad still emits a legal frame.
  (define (eth-tx nic-tx src-mac dst-mac ethertype payload plen)
    (let ((flen (+ 14 plen)))
      (let ((f (make-bytes (if (< flen 60) 60 flen))))
        (put-list! f 0 dst-mac)
        (put-list! f 6 src-mac)
        (put-be16! f 12 ethertype)
        (bytes-copy-into! f 14 payload plen)
        (send nic-tx (list 'tx f (bytes-length f))))))

  ;; --- ARP --------------------------------------------------------------------
  ;; ARP payload layout (28 bytes): htype(2) ptype(2) hlen(1) plen(1) oper(2)
  ;; sha(6) spa(4) tha(6) tpa(4).
  (define (build-arp oper src-mac src-ip tgt-mac tgt-ip)
    (let ((a (make-bytes 28)))
      (put-be16! a 0 #x0001)            ; htype = ethernet
      (put-be16! a 2 #x0800)            ; ptype = IPv4
      (bytes-u8-set! a 4 6)             ; hlen
      (bytes-u8-set! a 5 4)             ; plen
      (put-be16! a 6 oper)
      (put-list! a 8  src-mac)
      (put-list! a 14 src-ip)
      (put-list! a 18 tgt-mac)
      (put-list! a 24 tgt-ip)
      a))

  ;; A received ARP (frame; ethernet body at offset 14). Learn the sender, and if
  ;; it is a who-has for our IP, reply. Returns the new arp-cache.
  (define (handle-arp ip mac nic-tx cache frame len)
    (if (< len 42)
        cache
        (let ((oper   (get-be16 frame 20))
              (sha    (read-mac frame 22))
              (spa    (read-ip  frame 28))
              (tpa    (read-ip  frame 38)))
          (let ((cache2 (cache-put cache spa sha)))   ; learn sender regardless
            (if (and (= oper 1) (equal? tpa ip) mac nic-tx)
                (begin
                  (eth-tx nic-tx mac sha ETH-ARP
                          (build-arp 2 mac ip sha spa) 28)
                  cache2)
                cache2)))))

  (define (cache-put cache ip mac)
    (cons (cons (ip->key ip) mac)
          (filter (lambda (e) (not (= (car e) (ip->key ip)))) cache)))
  (define (cache-get cache ip)
    (let ((e (assq (ip->key ip) cache))) (if e (cdr e) #f)))

  ;; --- IPv4 build -------------------------------------------------------------
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
  (define (get-be16-list l off)         ; two bytes of a 4-byte ip list, BE
    (+ (arithmetic-shift (nth l off) 8) (nth l (+ off 1))))

  ;; --- ICMP -------------------------------------------------------------------
  ;; Echo message (8+ bytes): type(1) code(1) csum(2) id(2) seq(2) [data].
  (define (build-icmp-echo type id seq)
    (let ((m (make-bytes 8)))
      (bytes-u8-set! m 0 type)          ; 8 = request, 0 = reply
      (bytes-u8-set! m 1 0)             ; code
      (put-be16! m 4 id)
      (put-be16! m 6 seq)
      (put-be16! m 2 (csum m 0 8))      ; checksum over the whole message
      m))

  ;; Received IPv4 (frame; IP header at offset 14). Validate, then dispatch ICMP
  ;; echo-request -> echo-reply and UDP -> bound handler. Pure wrt the cache.
  (define (handle-ip ip mac nic-tx cache binds frame len)
    (let ((o 14))                       ; IP header start
      (if (or (< len 34) (not (= (bit-extract (u8 frame o) 4 4) 4)))
          'ignore
          (let ((ihl (* (bit-extract (u8 frame o) 0 4) 4))
                (proto (u8 frame (+ o 9)))
                (src-ip (read-ip frame (+ o 12)))
                (dst-ip (read-ip frame (+ o 16))))
            ;; Drop unless addressed to us with a valid header checksum (the
            ;; checksum over a good header, including its own field, folds to 0).
            (if (or (not (equal? dst-ip ip)) (not (= 0 (csum frame o ihl))))
                'ignore
                (let ((l4 (+ o ihl)))       ; transport header offset
                  (cond
                    ((= proto IP-ICMP)
                     (handle-icmp ip mac nic-tx cache src-ip frame l4 len))
                    ((= proto IP-UDP)
                     (handle-udp binds src-ip dst-ip (read-mac frame 6) frame l4 len))
                    (else 'ignore))))))))

  ;; ICMP at offset `l4`. Reply to an echo request by flipping the type and
  ;; recomputing the checksum over the same body. Needs the sender's MAC, which
  ;; we take from the cache (learned when it ARPed us / we ARPed it).
  (define (handle-icmp ip mac nic-tx cache src-ip frame l4 len)
    (let ((ilen (- len l4)))
      (if (or (< ilen 8) (not (= (u8 frame l4) 8)))   ; only echo-request
          'ignore
          (let ((dst-mac (cache-get cache src-ip)))
            (if (not dst-mac)
                'ignore                  ; no route back yet
                (let ((reply (copy-bytes frame l4 ilen)))
                  (bytes-u8-set! reply 0 0)            ; type = echo reply
                  (bytes-u8-set! reply 1 0)            ; code
                  (put-be16! reply 2 0)               ; clear checksum
                  (put-be16! reply 2 (csum reply 0 ilen))
                  (eth-tx nic-tx mac dst-mac ETH-IPV4
                          (build-ipv4 ip src-ip IP-ICMP reply ilen)
                          (+ 20 ilen))
                  'replied))))))

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
            ;; A non-zero checksum must verify (pseudo-header + segment fold to 0);
            ;; 0 means "no checksum" (RFC 768). Clamp the summed length to what we
            ;; actually received so a lying length can't over-read.
            (let ((seg (if (> seglen ulen) ulen seglen)))
              (if (and (not (= ucsum 0))
                       (not (= 0 (csum-seeded frame l4 seg
                                              (udp-pseudo-sum src-ip dst-ip seglen)))))
                  'ignore
                  (let ((h (assq dport binds)))
                    (if (not h)
                        'ignore
                        (let ((plen (- seg 8)))
                          (send (cdr h)
                                (list 'udp-rx src-ip src-mac sport
                                      (copy-bytes frame (+ l4 8) plen)))
                          'delivered)))))))))

  ;; Build + send a UDP datagram. A computed checksum of 0 is sent as 0xFFFF
  ;; (0 on the wire means "no checksum"); the pseudo-header + segment must sum to
  ;; zero at the receiver, so we seed the segment sum with the pseudo-header.
  (define (udp-send ip mac nic-tx dst-ip dst-mac sport dport payload plen)
    (let ((seg (+ 8 plen)))
      (let ((u (make-bytes seg)))
        (put-be16! u 0 sport)
        (put-be16! u 2 dport)
        (put-be16! u 4 seg)             ; UDP length
        (bytes-copy-into! u 8 payload plen)
        (let ((c (csum-seeded u 0 seg (udp-pseudo-sum ip dst-ip seg))))
          (put-be16! u 6 (if (= c 0) #xFFFF c)))
        ;; the ethernet payload is the whole IP packet (header + UDP segment)
        (eth-tx nic-tx mac dst-mac ETH-IPV4
                (build-ipv4 ip dst-ip IP-UDP u seg) (+ 20 seg)))))

  ;; --- the service ------------------------------------------------------------
  ;; State: (ip mac nic-tx arp-cache udp-binds). `serve` threads it; `me` (self)
  ;; lets a probe/lookup reply target be handed our own handle.
  (define (start-network-service our-ip)
    (serve (list our-ip #f #f '() '())
      (lambda (st m)
        (let ((ip (nth st 0)) (mac (nth st 1)) (tx (nth st 2))
              (cache (nth st 3)) (binds (nth st 4)))
          (cond
            ((eq? (car m) 'register-nic)         ; (register-nic mac tx-ctx)
             (display "[corenetwork] nic registered, mac=")
             (display (cadr m)) (newline)
             (list ip (cadr m) (caddr m) cache binds))
            ((eq? (car m) 'rx)                   ; (rx frame len)
             (let ((frame (cadr m)) (len (caddr m)))
               (if (< len 14)
                   st
                   (let ((etype (get-be16 frame 12)))
                     (cond
                       ((= etype ETH-ARP)
                        (list ip mac tx (handle-arp ip mac tx cache frame len) binds))
                       ((= etype ETH-IPV4)
                        (handle-ip ip mac tx cache binds frame len)
                        st)
                       (else st))))))
            ((eq? (car m) 'arp-request)          ; (arp-request ip)
             (if (and mac tx)
                 (eth-tx tx mac BROADCAST ETH-ARP
                         (build-arp 1 mac ip (list 0 0 0 0 0 0) (cadr m)) 28))
             st)
            ((eq? (car m) 'arp-lookup)           ; (arp-lookup ip reply)
             (send (caddr m) (cache-get cache (cadr m)))
             st)
            ((eq? (car m) 'udp-bind)             ; (udp-bind port handler)
             (display "[corenetwork] udp port bound: ")
             (display (cadr m)) (newline)
             (list ip mac tx cache (cons (cons (cadr m) (caddr m)) binds)))
            ((eq? (car m) 'udp-send)             ; (udp-send dst-ip dst-mac sport dport payload)
             (if (and mac tx)
                 (udp-send ip mac tx (cadr m) (caddr m) (cadddr m)
                           (nth m 4) (nth m 5) (bytes-length (nth m 5))))
             st)
            ((eq? (car m) 'ping)                 ; (ping dst-ip dst-mac id seq)
             (if (and mac tx)
                 (eth-tx tx mac (caddr m) ETH-IPV4
                         (build-ipv4 ip (cadr m) IP-ICMP
                                     (build-icmp-echo 8 (cadddr m) (nth m 4)) 8)
                         28))
             st)
            (else st)))))))
