;; corenetwork/dhcp: a DHCP client -- the interface configures itself (IP /
;; netmask / gateway / DNS) from the network instead of using a hardcoded
;; address. Ported from servers/CoreNetwork/src/dhcp.c.
;;
;; Two halves, mirroring the C: PURE helpers (dhcp-build / dhcp-parse, no I/O,
;; every option length bounds-checked) and the async driver -- a per-interface
;; client CONTEXT that binds UDP port 68, runs INIT -> SELECTING -> REQUESTING ->
;; BOUND -> RENEWING with exponential backoff, and writes the lease back to the
;; service with set-address. DHCP is a UDP protocol that needs no outbound ARP or
;; routing (DISCOVER/REQUEST are broadcast; renewal unicasts to the server's
;; learned source MAC), so it builds directly on the udp-send/udp-bind messages.
;;
;; BOOTP fixed header (240 bytes incl. the magic cookie), then TLV options:
;;   op(1) htype(1) hlen(1) hops(1) xid(4) secs(2) flags(2) ciaddr(4) yiaddr(4)
;;   siaddr(4) giaddr(4) chaddr(16) sname(64) file(128) cookie(4) options...

(define DHCP-CLIENT-PORT 68)
(define DHCP-SERVER-PORT 67)
(define DHCP-MAGIC-COOKIE #x63825363)
(define DHCP-FLAG-BROADCAST #x8000)
(define BOOTREQUEST 1)
(define BOOTREPLY 2)
(define DHCP-HDR 240)                  ; fixed header incl. cookie; options follow

;; message types (option 53)
(define DHCP-DISCOVER 1)
(define DHCP-OFFER 2)
(define DHCP-REQUEST 3)
(define DHCP-ACK 5)
(define DHCP-NAK 6)

;; option codes
(define OPT-PAD 0)
(define OPT-SUBNET 1)
(define OPT-ROUTER 3)
(define OPT-DNS 6)
(define OPT-REQ-IP 50)
(define OPT-LEASE 51)
(define OPT-MSGTYPE 53)
(define OPT-SERVER-ID 54)
(define OPT-PARAM 55)
(define OPT-END 255)

;; ===========================================================================
;; Pure helpers
;; ===========================================================================

;; The options block for a message: type (53) + optional requested-ip (50) and
;; server-id (54) for a selecting REQUEST + the param-request list (55) + end.
;; req-ip / server-id are 4-byte IP lists, or #f to omit.
(define (dhcp-options msg-type req-ip server-id)
  (append
    (list OPT-MSGTYPE 1 msg-type)
    (if req-ip (append (list OPT-REQ-IP 4) req-ip) '())
    (if server-id (append (list OPT-SERVER-ID 4) server-id) '())
    (list OPT-PARAM 4 OPT-SUBNET OPT-ROUTER OPT-DNS OPT-LEASE)
    (list OPT-END)))

;; Build a DHCP message of `msg-type` for `mac`/`xid` -> a bytes buffer (the UDP
;; payload). ciaddr (client IP, used when renewing) / req-ip / server-id are
;; 4-byte IP lists or #f. `broadcast` sets the flag asking the server to
;; broadcast its reply (we have no IP to unicast to yet).
(define (dhcp-build msg-type mac xid ciaddr req-ip server-id broadcast)
  (let* ((opts (dhcp-options msg-type req-ip server-id))
         (b (make-bytes (+ DHCP-HDR (length opts)))))
    (bytes-u8-set! b 0 BOOTREQUEST)
    (bytes-u8-set! b 1 1)              ; htype = ethernet
    (bytes-u8-set! b 2 6)             ; hlen = 6
    (put-be32! b 4 xid)
    (if broadcast (put-be16! b 10 DHCP-FLAG-BROADCAST))
    (if ciaddr (put-list! b 12 ciaddr))
    (put-list! b 28 mac)              ; chaddr (MAC in the first 6 bytes)
    (put-be32! b 236 DHCP-MAGIC-COOKIE)
    (put-list! b 240 opts)
    b))

;; Functional list element update (the options accumulator is an immutable list).
(define (list-set lst k v)
  (if (= k 0) (cons v (cdr lst))
      (cons (car lst) (list-set (cdr lst) (- k 1) v))))

;; A parse result is the 7-tuple (msg-type yiaddr server-id netmask gateway dns
;; lease); these name its slots.
(define (dhcp-type r)    (nth r 0))
(define (dhcp-yiaddr r)  (nth r 1))
(define (dhcp-server r)  (nth r 2))
(define (dhcp-netmask r) (nth r 3))
(define (dhcp-gateway r) (nth r 4))
(define (dhcp-dns r)     (nth r 5))
(define (dhcp-lease r)   (nth r 6))

(define (chaddr=? pl mac)
  (let loop ((i 0) (l mac))
    (cond ((null? l) #t)
          ((not (= (u8 pl (+ 28 i)) (car l))) #f)
          (else (loop (+ i 1) (cdr l))))))

;; Walk the TLV options from `off` to `end`, folding recognised options into the
;; accumulator. Every declared length is bounded against `end` before its bytes
;; are read (a lying length must not over-read and kill the context).
(define (dhcp-walk pl off end acc)
  (if (>= off end)
      acc
      (let ((code (u8 pl off)))
        (cond
          ((= code OPT-END) acc)
          ((= code OPT-PAD) (dhcp-walk pl (+ off 1) end acc))
          ((>= (+ off 1) end) acc)              ; missing length byte
          (else
           (let ((l (u8 pl (+ off 1))))
             (if (> (+ off 2 l) end)
                 acc                             ; declared length runs past buffer
                 (dhcp-walk pl (+ off 2 l) end
                            (dhcp-opt acc code l pl (+ off 2))))))))))

(define (dhcp-opt acc code l pl o)
  (cond
    ((and (= code OPT-MSGTYPE)   (>= l 1)) (list-set acc 0 (u8 pl o)))
    ((and (= code OPT-SERVER-ID) (>= l 4)) (list-set acc 2 (read-ip pl o)))
    ((and (= code OPT-SUBNET)    (>= l 4)) (list-set acc 3 (read-ip pl o)))
    ((and (= code OPT-ROUTER)    (>= l 4)) (list-set acc 4 (read-ip pl o)))
    ((and (= code OPT-DNS)       (>= l 4)) (list-set acc 5 (read-ip pl o)))
    ((and (= code OPT-LEASE)     (>= l 4)) (list-set acc 6 (get-be32 pl o)))
    (else acc)))

;; Parse `pl` (len bytes) as a DHCP reply for our `xid`/`mac`. Returns the
;; 7-tuple on success (a message type was present), or #f if it is not a reply
;; for us (bad op / cookie / xid / chaddr) or carried no message type.
(define (dhcp-parse pl len xid mac)
  (if (or (< len DHCP-HDR)
          (not (= (u8 pl 0) BOOTREPLY))
          (not (= (get-be32 pl 236) DHCP-MAGIC-COOKIE))
          (not (= (get-be32 pl 4) xid))
          (not (chaddr=? pl mac)))
      #f
      (let ((r (dhcp-walk pl DHCP-HDR len
                          (list 0 (read-ip pl 16) #f #f #f #f 0))))
        (if (= (dhcp-type r) 0) #f r))))         ; msg-type 0 = none seen

;; ===========================================================================
;; Async driver: per-interface client context
;; ===========================================================================

;; Transaction ids with no RNG: a per-module counter mixed with the MAC, so
;; successive transactions never reuse an id (which would match a stale reply).
(define dhcp-xid-ctr (make-cell 0))
(define (dhcp-xid mac)
  (cell-set! dhcp-xid-ctr (+ 1 (cell-ref dhcp-xid-ctr)))
  (let ((base (+ (arithmetic-shift (nth mac 2) 24)
                 (arithmetic-shift (nth mac 3) 16)
                 (arithmetic-shift (nth mac 4) 8) (nth mac 5))))
    (bitwise-and (bitwise-xor base (* (cell-ref dhcp-xid-ctr) 2654435761)) #xFFFFFFFF)))

;; Per-try wait window with exponential backoff (1s, 2s, 4s, ... cap 8s), in ns.
(define (dhcp-backoff t)
  (* (arithmetic-shift 1 (if (> t 3) 3 t)) 1000000000))

;; Non-blocking-until-deadline receive: pop the next message, or #f if none
;; arrives by `deadline` (uptime-ns). %mailbox-empty?/%mailbox-pop are the
;; non-blocking mailbox prims `recv` is built on; here we poll them with short
;; sleeps so a timeout can fire -- the analogue of the C's slot-poll loop.
(define (dhcp-poll deadline)
  (let loop ()
    (cond ((not (%mailbox-empty?)) (%mailbox-pop))
          ((> (uptime-ns) deadline) #f)
          (else (sleep 50000000) (loop)))))      ; 50ms poll

;; Await a DHCP reply for `xid` until `deadline`. If `want-type` is set only that
;; type satisfies (others are consumed and ignored); #f accepts any reply.
;; Non-udp messages are ignored. Returns (result . server-mac) or #f.
(define (dhcp-await mac xid want-type deadline)
  (let loop ()
    (let ((msg (dhcp-poll deadline)))
      (if (not msg)
          #f
          (if (not (eq? (car msg) 'udp-rx))      ; (udp-rx src-ip src-mac sport payload)
              (loop)
              (let ((src-mac (caddr msg)) (pl (nth msg 4)))
                (let ((r (dhcp-parse pl (bytes-length pl) xid mac)))
                  (if (and r (or (not want-type) (= (dhcp-type r) want-type)))
                      (cons r src-mac)
                      (loop)))))))))

;; Send a message and await a reply, retrying with backoff up to 5 times.
;; dst-ip/dst-mac select broadcast vs. unicast (renew). Returns (result . mac).
(define (dhcp-exchange net mac xid msg-type ciaddr req-ip server-id broadcast
                       dst-ip dst-mac want-type)
  (let try ((t 0))
    (if (>= t 5)
        #f
        (begin
          (send net (list 'udp-send dst-ip dst-mac DHCP-CLIENT-PORT DHCP-SERVER-PORT
                          (dhcp-build msg-type mac xid ciaddr req-ip server-id broadcast)))
          (let ((g (dhcp-await mac xid want-type (+ (uptime-ns) (dhcp-backoff t)))))
            (if g g (try (+ t 1))))))))

(define (dhcp-lease-or r default)
  (if (> (dhcp-lease r) 0) (dhcp-lease r) default))

(define (dhcp-log ack)
  (display "[DHCP] bound ") (display (dhcp-yiaddr ack))
  (display " gw ") (display (dhcp-gateway ack))
  (display " mask ") (display (dhcp-netmask ack))
  (display " lease ") (display (dhcp-lease ack)) (display "s") (newline))

(define (dhcp-set net r)
  (send net (list 'set-address (dhcp-yiaddr r) (dhcp-netmask r)
                  (dhcp-gateway r) (dhcp-dns r))))

;; Idle until `deadline` (uptime-ns), discarding any frames that land on our
;; bound port meanwhile. A bare (sleep T1) is NOT safe here: `sleep` parks on the
;; same blocked flag `recv` uses, and a `send` clears it, so any stray datagram
;; to port 68 would wake the renew wait early and spin it. We instead poll to the
;; real deadline and drop idle-time mail (no reply is expected until we REQUEST).
(define (dhcp-idle deadline)
  (let ((now (uptime-ns)))
    (cond ((>= now deadline) 'done)
          ((not (%mailbox-empty?)) (%mailbox-pop) (dhcp-idle deadline))
          ;; sleep the whole remaining interval; a stray frame wakes us early, in
          ;; which case the next loop drops it and re-sleeps what time is left.
          (else (sleep (- deadline now)) (dhcp-idle deadline)))))

;; BOUND: apply the lease, then renew at T1 (lease/2) by unicasting REQUEST to
;; the server we learned the MAC of. A renewal NAK or timeout falls back to the
;; full DISCOVER cycle via (restart).
(define (dhcp-bound net mac ack server-mac restart)
  (dhcp-set net ack)
  (dhcp-log ack)
  (let renew ((lease (dhcp-lease-or ack 3600))
              (sid (dhcp-server ack)))
    (dhcp-idle (+ (uptime-ns) (* (quotient lease 2) 1000000000)))   ; T1 = lease/2 s
    (let ((rep (dhcp-exchange net mac (dhcp-xid mac) DHCP-REQUEST
                              (dhcp-yiaddr ack) #f #f #f sid server-mac #f)))
      (if (or (not rep) (= (dhcp-type (car rep)) DHCP-NAK))
          (restart)                              ; lease lost -> re-DISCOVER
          (begin (dhcp-set net (car rep))
                 (renew (dhcp-lease-or (car rep) lease) sid))))))

;; The client context: bind port 68, then loop the state machine forever.
(define (dhcp-client net mac)
  (send net (list 'udp-bind DHCP-CLIENT-PORT (self)))
  (let restart ()
    ;; One transaction id for the whole SELECTING->REQUESTING handshake: the
    ;; REQUEST must echo the DISCOVER/OFFER xid (RFC 2131 4.3.2) so the server
    ;; matches it to the offer it made. (Renewal below starts a fresh xid -- it is
    ;; a new transaction.)
    (let ((xid (dhcp-xid mac)))
      ;; SELECTING: broadcast DISCOVER, await OFFER.
      (let ((offer (dhcp-exchange net mac xid DHCP-DISCOVER
                                  #f #f #f #t IP-BROADCAST BROADCAST DHCP-OFFER)))
        (if (not offer)
            (begin (sleep 5000000000) (restart))   ; nobody answered; retry cycle
            ;; REQUESTING: broadcast REQUEST(server-id, requested-ip), await ACK/NAK.
            (let ((off (car offer)))
              (let ((rep (dhcp-exchange net mac xid DHCP-REQUEST
                                        #f (dhcp-yiaddr off) (dhcp-server off) #t
                                        IP-BROADCAST BROADCAST #f)))
                (if (or (not rep) (= (dhcp-type (car rep)) DHCP-NAK))
                    (begin (sleep 2000000000) (restart))
                    (dhcp-bound net mac (car rep) (cdr rep) restart)))))))))
