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

;; Cache entries age out: a learned (ip -> mac) is good for ARP-CACHE-TTL-NS
;; (120s, the C arp_cache TTL), after which a lookup reports a miss so the next
;; send re-resolves. `now` is a monotonic nanosecond stamp (uptime-ns); when no
;; clock is calibrated it reads 0, and a 0 expiry means never-expire -- the same
;; graceful degradation the C had.
(define ARP-CACHE-TTL-NS 120000000000)        ; 120 * 1e9

;; A received ARP (frame; ethernet body at offset 14). Learn the sender, and if
;; it is a who-has for our IP, reply. Returns the new arp-cache. `now` stamps the
;; learned entry's expiry.
(define (handle-arp ip mac nic-tx cache now frame len)
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
        (let ((cache2 (cache-put cache spa sha now)))   ; learn sender regardless
          (if (= oper 2)                            ; a reply we solicited / observed
              (begin (display "[corenetwork] arp learned ") (display spa)
                     (display " -> ") (display sha) (newline)))
          (if (and (= oper 1) (equal? tpa ip) mac nic-tx)
              (begin
                (eth-tx nic-tx mac sha ETH-ARP
                        (build-arp 2 mac ip sha spa) 28)
                cache2)
              cache2)))))

;; A cache entry is (key mac . expiry). expiry 0 = never expire (no clock).
(define (entry-mac e) (cadr e))
(define (entry-expiry e) (cddr e))
(define (arp-entry-expired? e now)
  (and (not (= (entry-expiry e) 0)) (> now (entry-expiry e))))

(define (cache-put cache ip mac now)
  (let ((expiry (if (= now 0) 0 (+ now ARP-CACHE-TTL-NS))))
    (cons (cons (ip->key ip) (cons mac expiry))
          (filter (lambda (e) (not (= (car e) (ip->key ip)))) cache))))
;; Lookup honours expiry: an aged-out entry reports a miss (it lingers in the
;; immutable list until re-learned, but is never returned). `now` is the
;; monotonic stamp to compare against.
(define (cache-get cache ip now)
  (let ((e (assq (ip->key ip) cache)))
    (if (and e (not (arp-entry-expired? e now))) (entry-mac e) #f)))

;; --- outbound resolution (client side) -------------------------------------
;; arp-resolve next-hop <ip> -> its MAC list, or #f if unreachable. The caller
;; supplies the next-hop IP (fully decoupled from routing, exactly as the C
;; arp_resolve was). Fast path: a cache hit returns at once. On a miss it
;; broadcasts a who-has and polls the cache (which the service fills from any
;; ARP reply it sees), ~4 tries / ~1s, then gives up.
;;
;; SYNCHRONOUS and task-context only: it blocks on `recv` for the service's
;; arp-lookup reply and `sleep`s between tries, so it must run in its own
;; context (the message it consumes is the lookup answer it just asked for) --
;; never inside the service loop. This mirrors the C contract.
(define (arp-resolve net ip)
  (let loop ((tries 4))
    (send net (list 'arp-lookup ip (self)))
    (let ((mac (recv)))
      (cond (mac mac)                          ; cache hit (or a reply landed)
            ((= tries 0) #f)                    ; gave up
            (else
             (send net (list 'arp-request ip))  ; solicit, let a reply land
             (sleep 250000000)                  ; 250ms, ~1s over 4 tries
             (loop (- tries 1)))))))
