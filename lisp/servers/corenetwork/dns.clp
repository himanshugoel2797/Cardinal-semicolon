;; corenetwork/dns: a minimal DNS resolver (A records over UDP/53). Like the DHCP
;; client it is a self-contained context that builds on udp-bind/udp-send/udp-rx:
;; it binds an ephemeral port, sends a query to the DHCP-learned DNS server, and
;; polls its mailbox to a deadline (the same non-blocking pattern as dhcp-poll, so
;; a lost reply times out and retransmits). `dns-resolve` is SYNCHRONOUS and
;; task-context only (it blocks via the poll loop) -- run it in its own context.
;;
;; Next-hop selection here is minimal (on-link DNS server -> resolve it directly,
;; else via the gateway); the routing-table work will centralise this. Only A
;; (IPv4) records are parsed; CNAME chains are followed only insofar as the server
;; flattens them into the answer (slirp and real resolvers do).

(define DNS-PORT 53)

;; "a.b.c" -> the wire QNAME (length-prefixed labels + a 0 terminator) as bytes.
(define (dns-split-dots codes)          ; codes: list of char codes -> list of labels
  (let loop ((cs codes) (cur '()) (acc '()))
    (cond ((null? cs) (reverse (cons (reverse cur) acc)))
          ((= (car cs) 46) (loop (cdr cs) '() (cons (reverse cur) acc)))  ; '.'
          (else (loop (cdr cs) (cons (car cs) cur) acc)))))

(define (dns-encode-name name)
  (let* ((labels (dns-split-dots (map char->integer (string->list name))))
         ;; each label -> (len byte...) ; then a trailing 0
         (blist (append (apply append (map (lambda (l) (cons (length l) l)) labels))
                        (list 0)))
         (b (make-bytes (length blist))))
    (let loop ((i 0) (l blist))
      (if (null? l) b
          (begin (bytes-u8-set! b i (car l)) (loop (+ i 1) (cdr l)))))))

;; A standard recursive A-record query for `name` with id `qid`.
(define (build-dns-query qid name)
  (let* ((qname (dns-encode-name name))
         (qn (bytes-length qname))
         (q (make-bytes (+ 12 qn 4))))
    (put-be16! q 0 qid)
    (put-be16! q 2 #x0100)               ; flags: RD (recursion desired)
    (put-be16! q 4 1)                    ; QDCOUNT = 1
    (bytes-copy-into! q 12 qname qn)
    (put-be16! q (+ 12 qn) 1)            ; QTYPE  = A
    (put-be16! q (+ 12 qn 2) 1)          ; QCLASS = IN
    q))

;; Offset just past the name at `off`: walk labels until a 0 terminator, or stop
;; at a 2-byte compression pointer (top two bits set). Bounded by `len` so a
;; malformed packet returns `len` (which the callers treat as out-of-range) rather
;; than reading past the buffer and killing the resolver context.
(define (dns-skip-name buf len off)
  (let loop ((o off))
    (if (>= o len)
        len
        (let ((b (bytes-u8-ref buf o)))
          (cond ((= b 0) (+ o 1))
                ((>= b #xC0) (+ o 2))    ; compression pointer ends the name
                (else (loop (+ o 1 b))))))))

;; Parse a response: verify the id, skip the question(s), then scan answers for
;; the first A record, returning its IPv4 as a 4-element list (or #f). Every
;; offset is bounds-checked against `len`.
(define (parse-dns-answer buf qid)
  (let ((len (bytes-length buf)))
    (if (or (< len 12) (not (= (get-be16 buf 0) qid)))
        #f
        (let ((qd (get-be16 buf 4)) (an (get-be16 buf 6)))
          (if (= an 0)
              #f
              ;; skip the question section: qd names, each + 4 (qtype, qclass)
              (let skipq ((i 0) (o 12))
                (if (< i qd)
                    (skipq (+ i 1) (+ (dns-skip-name buf len o) 4))
                    ;; scan the answer RRs
                    (let ansloop ((i 0) (o o))
                      (let ((no (dns-skip-name buf len o)))
                        (if (or (>= i an) (> (+ no 10) len))
                            #f
                            (let ((type (get-be16 buf no))
                                  (rdlen (get-be16 buf (+ no 8)))
                                  (rdata (+ no 10)))
                            (if (> (+ rdata rdlen) len)
                                #f
                                (if (and (= type 1) (= rdlen 4))   ; A record
                                    (list (bytes-u8-ref buf rdata) (bytes-u8-ref buf (+ rdata 1))
                                          (bytes-u8-ref buf (+ rdata 2)) (bytes-u8-ref buf (+ rdata 3)))
                                    (ansloop (+ i 1) (+ rdata rdlen)))))))))))))))

;; True if a and b share the network under `mask` (octet-wise AND compare).
(define (dns-same-subnet? a b mask)
  (let loop ((i 0))
    (cond ((= i 4) #t)
          ((not (= (bitwise-and (nth a i) (nth mask i))
                   (bitwise-and (nth b i) (nth mask i)))) #f)
          (else (loop (+ i 1))))))

;; Non-blocking-until-deadline mailbox pop (the dhcp-poll pattern).
(define (dns-poll deadline)
  (let loop ()
    (cond ((not (%mailbox-empty?)) (%mailbox-pop))
          ((> (uptime-ns) deadline) #f)
          (else (sleep 50000000) (loop)))))      ; 50ms poll

;; Send the query and await a matching reply, retrying with 1s/2s/4s backoff.
(define (dns-query-loop net dns mac sport qid name)
  (let try ((t 0))
    (if (>= t 3)
        #f
        (begin
          (send net (list 'udp-send dns mac sport DNS-PORT (build-dns-query qid name)))
          (let await ((deadline (+ (uptime-ns) (* (arithmetic-shift 1 t) 1000000000))))
            (let ((m (dns-poll deadline)))
              (cond
                ((not m) (try (+ t 1)))                    ; timeout -> retransmit
                ((and (eq? (car m) 'udp-rx) (= (nth m 3) DNS-PORT))
                 (let ((r (parse-dns-answer (nth m 4) qid)))
                   (if r r (await deadline))))             ; a DNS reply (maybe no A)
                (else (await deadline)))))))))             ; unrelated mail -> keep waiting

;; Resolve `name` (a string) to an IPv4 address (a 4-element list), or #f on
;; failure / no DNS server / unreachable next hop. SYNCHRONOUS, own-context only.
(define (dns-resolve net name)
  (send net (list 'get-address (self)))
  (let* ((cfg (recv)) (ip (nth cfg 0)) (nm (nth cfg 1)) (gw (nth cfg 2)) (dns (nth cfg 3)))
    (if (or (equal? dns IP-ANY) (equal? ip IP-ANY))
        #f                                                  ; interface not configured
        (let* ((nexthop (if (dns-same-subnet? dns ip nm) dns gw))
               (mac (arp-resolve net nexthop)))
          (if (not mac)
              #f
              (let ((qid (bitwise-and (uptime-ns) #xFFFF))
                    (sport (+ 30000 (modulo (uptime-ns) 20000))))  ; ephemeral source port
                (send net (list 'udp-bind sport (self)))
                (let ((result (dns-query-loop net dns mac sport qid name)))
                  (send net (list 'udp-unbind sport))   ; release the ephemeral port
                  result)))))))
