;; corenetwork/txq: the async transmit path with ARP packet-hold. Lets a caller
;; send to an arbitrary destination WITHOUT first resolving the next-hop MAC: the
;; service routes the packet, looks up the next hop in the ARP cache, and either
;; sends it immediately (cache hit) or HOLDS it, emits an ARP who-has, and flushes
;; it when the reply lands. Packets to the same unresolved next hop coalesce behind
;; one ARP request; an entry that stays unresolved is retried a few times and then
;; dropped. This is the fire-and-forget alternative to the caller-driven,
;; per-context, blocking `arp-resolve` + caller-supplies-MAC pattern.
;;
;; State is a mutable cell in the service closure holding a list of PENDING entries,
;; one per distinct next hop:
;;   (next-hop iface frames tries next-arp drop)
;; where `frames` is a list of (etype frame flen) held for that next hop, `tries`
;; counts ARP attempts, `next-arp` is when to re-who-has, and `drop` is the deadline
;; after which the held frames are discarded. ARP resolution is driven by the ARP
;; rx path (flush) and the periodic tick (retry/timeout).

(define TXQ-ARP-INTERVAL-NS 500000000)   ; re-ARP an unresolved next hop every 500ms
(define TXQ-DROP-NS 3000000000)          ; give up (drop held frames) after 3s

;; The queue is a mutable cell holding the list of pending entries. (make-cell is
;; now vector-backed, so it safely holds a list and is GC-traced -- see driver-util.)
(define (mk-txq) (make-cell '()))
(define (txq-get txq) (cell-ref txq))
(define (txq-put! txq v) (cell-set! txq v))

;; Pending-entry accessors (a plain list; rebuilt functionally on change).
(define (pe-nexthop e) (nth e 0))
(define (pe-iface e)   (nth e 1))
(define (pe-frames e)  (nth e 2))
(define (pe-tries e)   (nth e 3))
(define (pe-next-arp e)(nth e 4))
(define (pe-drop e)    (nth e 5))

(define (txq-find holds nh)
  (cond ((null? holds) #f)
        ((equal? (pe-nexthop (car holds)) nh) (car holds))
        (else (txq-find (cdr holds) nh))))
(define (txq-remove holds nh)
  (filter (lambda (e) (not (equal? (pe-nexthop e) nh))) holds))

;; Emit an ARP who-has for `nh` out `iface`.
(define (txq-who-has iface nh)
  (eth-tx (ig iface I-TX) (ig iface I-MAC) BROADCAST ETH-ARP
          (build-arp 1 (ig iface I-MAC) (ig iface I-IP) (list 0 0 0 0 0 0) nh) 28))

;; Transmit one held frame (etype frame flen) to `mac` out `iface`. An IPv4 frame
;; goes via eth-tx-ip so an over-MTU held datagram still fragments on flush.
(define (txq-emit iface mac fr)
  (if (= (nth fr 0) ETH-IPV4)
      (eth-tx-ip (ig iface I-TX) (ig iface I-MAC) mac (nth fr 1) (nth fr 2))
      (eth-tx (ig iface I-TX) (ig iface I-MAC) mac (nth fr 0) (nth fr 1) (nth fr 2))))

;; Send `(etype frame flen)` to `next-hop` via `iface`: if the MAC is cached, go
;; now; otherwise hold the frame and (if this is the first one for that next hop)
;; emit a who-has. Mutates the txq cell.
(define (txq-send! txq cache now iface next-hop etype frame flen)
  (let ((mac (cache-get cache next-hop now)) (fr (list etype frame flen)))
    (if mac
        (txq-emit iface mac fr)                  ; cache hit: go now, queue unchanged
        (let* ((holds (txq-get txq)) (e (txq-find holds next-hop)))
          (if e
              ;; already resolving this next hop: just append (coalesce, no ARP)
              (txq-put! txq (cons (list next-hop iface (cons fr (pe-frames e))
                                         (pe-tries e) (pe-next-arp e) (pe-drop e))
                                   (txq-remove holds next-hop)))
              (begin
                (txq-who-has iface next-hop)
                (txq-put! txq (cons (list next-hop iface (list fr) 1
                                           (+ now TXQ-ARP-INTERVAL-NS) (+ now TXQ-DROP-NS))
                                     holds))))))))

;; ARP learned something: flush every entry whose next hop is now resolvable,
;; transmitting its held frames (newest-first order does not matter for UDP).
(define (txq-flush! txq cache now)
  (let loop ((holds (txq-get txq)) (keep '()))
    (if (null? holds)
        (txq-put! txq keep)
        (let* ((e (car holds)) (mac (cache-get cache (pe-nexthop e) now)))
          (if mac
              (begin (for-each (lambda (fr) (txq-emit (pe-iface e) mac fr)) (pe-frames e))
                     (loop (cdr holds) keep))
              (loop (cdr holds) (cons e keep)))))))

;; Periodic maintenance: drop entries past their deadline (next hop never
;; answered), and re-emit a who-has for the rest when their retry interval elapses.
(define (txq-tick! txq now)
  (let loop ((holds (txq-get txq)) (keep '()))
    (if (null? holds)
        (txq-put! txq keep)
        (let ((e (car holds)))
          (cond
            ((> now (pe-drop e)) (loop (cdr holds) keep))   ; timed out: drop
            ((> now (pe-next-arp e))
             (txq-who-has (pe-iface e) (pe-nexthop e))
             (loop (cdr holds)
                   (cons (list (pe-nexthop e) (pe-iface e) (pe-frames e)
                               (+ (pe-tries e) 1) (+ now TXQ-ARP-INTERVAL-NS) (pe-drop e))
                         keep)))
            (else (loop (cdr holds) (cons e keep))))))))
