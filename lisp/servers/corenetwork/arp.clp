;; corenetwork/arp: ARP request/reply + the address cache.
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
  ;; Only ethernet/IPv4 ARP with 6-byte MACs and 4-byte IPs (as the C arp_rx
  ;; required): a non-matching htype/ptype/hlen/plen would otherwise be read
  ;; with the wrong fixed offsets and poison the cache.
  (if (or (< len 42)
          (not (= (get-be16 frame 14) #x0001))   ; htype = ethernet
          (not (= (get-be16 frame 16) #x0800))   ; ptype = IPv4
          (not (= (u8 frame 18) 6))              ; hlen
          (not (= (u8 frame 19) 4)))             ; plen
      cache
      (let ((oper   (get-be16 frame 20))
            (sha    (read-mac frame 22))
            (spa    (read-ip  frame 28))
            (tpa    (read-ip  frame 38)))
        (let ((cache2 (cache-put cache spa sha)))   ; learn sender regardless
          (if (= oper 2)                            ; a reply we solicited / observed
              (begin (display "[corenetwork] arp learned ") (display spa)
                     (display " -> ") (display sha) (newline)))
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
