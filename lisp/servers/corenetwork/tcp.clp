;; corenetwork/tcp: a small but real TCP/IP implementation -- active + passive
;; open, in-order data with cumulative ACKs, out-of-order reassembly, timeout
;; retransmission (Go-Back-N), the FIN teardown handshake with TIME-WAIT, the
;; pseudo-header checksum, and peer-window flow control. It deliberately omits
;; congestion control (LAN/loopback target), SACK, window scaling, and timestamps.
;;
;; All connection state lives in the single network SERVICE context (no locks):
;; every TCP event is a message the service serializes. Connections are kept in
;; mutable hash tables (by 4-tuple for rx demux, by id for the socket API) and in
;; mutable connection VECTORS -- the in-place state a TCP needs, which the
;; immutable core gained for exactly this (see lisp/vm mutable containers).
;;
;; Timers: the service can't block, so a ticker context sends (tcp-tick) every
;; TICK-MS; the tick handler walks connections to retransmit and to expire
;; TIME-WAIT. A received segment replies on the captured src-mac (responder-style,
;; no ARP); an actively-opened connection stores the next-hop MAC the caller
;; resolved.
;;
;; Socket API (messages to the service; see service.clp for the wiring):
;;   (tcp-listen <port> <owner>)                 ; passive open
;;   (tcp-connect <dst-ip> <dst-mac> <dport> <owner>)  ; active open
;;   (tcp-send <conn> <bytes>)                   ; queue data
;;   (tcp-close <conn>)                          ; begin active close
;; The owner context receives:
;;   (tcp-accept <lport> <conn> <rip> <rport>)   ; an inbound connection established
;;   (tcp-connected <conn>)                      ; our active open completed
;;   (tcp-rx <conn> <bytes>)                     ; in-order data
;;   (tcp-closed <conn>)                         ; peer closed (or the conn aborted)

(define IP-TCP 6)

;; Flag bits (byte 13 of the TCP header).
(define TCP-FIN 1) (define TCP-SYN 2) (define TCP-RST 4)
(define TCP-PSH 8) (define TCP-ACK 16)

(define TCP-MSS 1024)        ; advertised + send segmentation size
(define RCV-WND 16384)       ; fixed advertised receive window
(define RTO-INIT 500000000)  ; 500ms initial retransmit timeout (ns)
(define RTO-MAX 8000000000)  ; 8s cap
(define MSL-NS 2000000000)   ; 2s "maximum segment lifetime"; TIME-WAIT = 2*MSL
(define TICK-MS 100)         ; retransmit/timeout granularity

;; --- 32-bit sequence arithmetic (mod 2^32, wraparound-correct) --------------
(define (seq+ a b) (bitwise-and (+ a b) #xFFFFFFFF))
(define (seq- a b) (bitwise-and (- a b) #xFFFFFFFF))
(define (seq< a b) (>= (seq- a b) #x80000000))   ; signed difference, high bit set
(define (seq<= a b) (or (= a b) (seq< a b)))
(define (seq> a b) (seq< b a))
(define (tcp-min a b) (if (< a b) a b))

;; --- connection record (a mutable vector) -----------------------------------
(define C-STATE 0)     ; symbol: syn-sent syn-rcvd established close-wait
                       ;         fin-wait-1 fin-wait-2 closing last-ack time-wait
(define C-ID 1)        ; integer handle (the socket id given to the owner)
(define C-OWNER 2)     ; owner context (where tcp-rx / events are delivered)
(define C-LPORT 3)
(define C-RIP 4)       ; remote ip (4-element list)
(define C-RPORT 5)
(define C-RMAC 6)      ; remote next-hop mac (6-element list)
(define C-ISS 7)       ; our initial send sequence
(define C-SND-UNA 8)   ; oldest unacknowledged seq
(define C-SND-NXT 9)   ; next seq to send
(define C-SND-WND 10)  ; peer's advertised window
(define C-DBASE 11)    ; seq number of sndq byte 0 (= iss+1, advances as data acks)
(define C-SNDQ 12)     ; bytes: unacknowledged + unsent outgoing data
(define C-RCV-NXT 13)  ; next expected receive seq
(define C-OOO 14)      ; out-of-order segments: sorted list of (seq fin . bytes)
(define C-FINQ 15)     ; #t once the owner has requested close
(define C-FIN-SENT 16) ; #t once our FIN is on the wire
(define C-FIN-SEQ 17)  ; seq our FIN occupies, or #f
(define C-RTX 18)      ; retransmit deadline (ns), 0 = timer off
(define C-RTO 19)      ; current RTO (ns), doubles on timeout
(define C-TW 20)       ; TIME-WAIT expiry (ns), 0 = not in TIME-WAIT
(define C-LEN 21)

(define (cg c i) (vector-ref c i))
(define (cs! c i v) (vector-set! c i v))

(define (mk-conn id owner lport rip rport rmac iss)
  (let ((c (make-vector C-LEN 0)))
    (cs! c C-STATE 'closed) (cs! c C-ID id) (cs! c C-OWNER owner)
    (cs! c C-LPORT lport) (cs! c C-RIP rip) (cs! c C-RPORT rport) (cs! c C-RMAC rmac)
    (cs! c C-ISS iss) (cs! c C-SND-UNA iss) (cs! c C-SND-NXT iss) (cs! c C-SND-WND RCV-WND)
    (cs! c C-DBASE (seq+ iss 1)) (cs! c C-SNDQ (make-bytes 0))
    (cs! c C-RCV-NXT 0) (cs! c C-OOO '())
    (cs! c C-FINQ #f) (cs! c C-FIN-SENT #f) (cs! c C-FIN-SEQ #f)
    (cs! c C-RTX 0) (cs! c C-RTO RTO-INIT) (cs! c C-TW 0)
    c))

;; --- TCP state for the service (one per interface) --------------------------
;; A vector: by-tuple hash (key (lport rip rport) -> conn), by-id hash (id ->
;; conn), listeners hash (lport -> owner), a next-id counter, and a test-only
;; inbound-loss config (drop denominator + a segment counter; 0 = no loss).
(define T-TUPLE 0) (define T-ID 1) (define T-LISTEN 2) (define T-NEXT 3)
(define T-LOSS 4) (define T-CTR 5)
(define (mk-tcp-state)
  (vector (make-hash-table) (make-hash-table) (make-hash-table) 1 0 0))

;; Test-only fault injection (enabled by cardinal.tcploss via tcp-do-test-loss):
;; drop 1 in N received segments, EXCEPT SYNs (so the handshake still completes),
;; to exercise the paths a lossless link never reaches -- dropping a data segment
;; opens a gap that drives out-of-order reassembly, and dropping an ACK leaves our
;; sent data unacknowledged so our retransmit timer fires. Off (N=0) by default,
;; one comparison per segment, so it is inert in production.
(define (tcp-test-drop? tcp synf)
  (let ((n (cg tcp T-LOSS)))
    (and (> n 0) (not synf)
         (begin (cs! tcp T-CTR (+ (cg tcp T-CTR) 1))
                (if (= 0 (modulo (cg tcp T-CTR) n))
                    (begin (display "[tcp-test] dropped inbound segment") (newline) #t)
                    #f)))))
(define (tcp-do-test-loss tcp n) (cs! tcp T-LOSS n))
(define (tcp-tuple lport rip rport) (list lport rip rport))
(define (tcp-conn-add tcp c)
  (hash-set! (cg tcp T-TUPLE) (tcp-tuple (cg c C-LPORT) (cg c C-RIP) (cg c C-RPORT)) c)
  (hash-set! (cg tcp T-ID) (cg c C-ID) c))
(define (tcp-conn-del tcp c)
  (hash-remove! (cg tcp T-TUPLE) (tcp-tuple (cg c C-LPORT) (cg c C-RIP) (cg c C-RPORT)))
  (hash-remove! (cg tcp T-ID) (cg c C-ID)))
(define (tcp-conn-by-tuple tcp lport rip rport)
  (hash-ref (cg tcp T-TUPLE) (tcp-tuple lport rip rport) #f))
(define (tcp-conn-by-id tcp id) (hash-ref (cg tcp T-ID) id #f))
(define (tcp-next-id tcp)
  (let ((id (cg tcp T-NEXT))) (cs! tcp T-NEXT (+ id 1)) id))

;; --- segment construction + transmit ----------------------------------------
(define (tcp-pseudo-sum sip dip tcplen)
  (+ (get-be16-list sip 0) (get-be16-list sip 2)
     (get-be16-list dip 0) (get-be16-list dip 2) IP-TCP tcplen))

;; Build a TCP segment (header [+ MSS option] + payload) with a valid checksum.
;; `payload` is a bytes buffer or #f; `with-mss` adds the 4-byte MSS option (SYNs).
(define (tcp-build sport dport seq ack flags window sip dip payload with-mss)
  (let* ((plen (if payload (bytes-length payload) 0))
         (hl (if with-mss 24 20))
         (seg (make-bytes (+ hl plen))))
    (put-be16! seg 0 sport)
    (put-be16! seg 2 dport)
    (put-be32! seg 4 seq)
    (put-be32! seg 8 ack)
    (bytes-u8-set! seg 12 (arithmetic-shift (quotient hl 4) 4))  ; data offset, high nibble
    (bytes-u8-set! seg 13 flags)
    (put-be16! seg 14 window)
    (if with-mss
        (begin (bytes-u8-set! seg 20 2) (bytes-u8-set! seg 21 4)
               (put-be16! seg 22 TCP-MSS)))
    (if (> plen 0) (bytes-copy-into! seg hl payload plen))
    (let ((c (csum-seeded seg 0 (+ hl plen) (tcp-pseudo-sum sip dip (+ hl plen)))))
      (put-be16! seg 16 (if (= c 0) #xFFFF c)))
    seg))

;; Send a segment for connection `c`. `payload` is a bytes slice or #f.
(define (tcp-xmit ip mac tx c seq ack flags payload with-mss)
  (let* ((seg (tcp-build (cg c C-LPORT) (cg c C-RPORT) seq ack flags RCV-WND
                         ip (cg c C-RIP) payload with-mss))
         (slen (bytes-length seg)))
    (eth-tx tx mac (cg c C-RMAC) ETH-IPV4
            (build-ipv4 ip (cg c C-RIP) IP-TCP seg slen) (+ 20 slen))))

;; A RST aimed back at a segment we have no connection for (or to abort), built
;; from the offending frame's addressing -- responder style. Follows RFC 793's
;; reset rules: if the offending segment carried an ACK, reply RST with seq =
;; their ack (no ACK flag); otherwise reply RST+ACK with seq 0 and ack = their
;; seq + their segment's sequence span (so a SYN to a closed port is refused with
;; a well-formed RST+ACK that names the next expected seq). `sport`/`dport` are
;; already our (source, dest) ports for the reply.
(define (tcp-send-rst ip mac tx dst-ip dst-mac sport dport in-seq in-ack in-span ackflag)
  (let* ((seg (if ackflag
                  (tcp-build sport dport in-ack 0 TCP-RST 0 ip dst-ip #f #f)
                  (tcp-build sport dport 0 (seq+ in-seq in-span)
                             (bitwise-or TCP-RST TCP-ACK) 0 ip dst-ip #f #f)))
         (slen (bytes-length seg)))
    (eth-tx tx mac dst-mac ETH-IPV4 (build-ipv4 ip dst-ip IP-TCP seg slen) (+ 20 slen))))

;; --- the rtx timer ----------------------------------------------------------
;; Arm only when data/SYN/FIN is outstanding (snd-una != snd-nxt); disarm when
;; everything is acknowledged. now is uptime-ns.
(define (tcp-arm! c now)
  (if (= (cg c C-SND-UNA) (cg c C-SND-NXT))
      (cs! c C-RTX 0)                                  ; nothing outstanding
      (if (= (cg c C-RTX) 0)
          (cs! c C-RTX (+ now (cg c C-RTO))))))        ; start if not already running
(define (tcp-rearm! c now) (cs! c C-RTX 0) (tcp-arm! c now))

;; --- transmit unsent data (and the FIN) within the window -------------------
;; Sends new segments from snd-nxt up to the window/queue limit; emits the FIN
;; once all queued data has been sent and the owner has asked to close.
(define (tcp-output ip mac tx c now)
  (let* ((dbase (cg c C-DBASE))
         (data-end (seq+ dbase (bytes-length (cg c C-SNDQ))))  ; seq just past last data byte
         (win (if (> (cg c C-SND-WND) 0) (cg c C-SND-WND) 1))  ; >=1: a zero-window probe
         (limit (let ((we (seq+ (cg c C-SND-UNA) win)))
                  (if (seq< we data-end) we data-end))))       ; min(win-end, data-end)
    (let loop ()
      (let ((nxt (cg c C-SND-NXT)))
        (if (seq< nxt limit)
            (let* ((avail (seq- limit nxt))
                   (n (tcp-min TCP-MSS avail))
                   (off (seq- nxt dbase))
                   (slice (copy-bytes (cg c C-SNDQ) off n)))
              (tcp-xmit ip mac tx c nxt (cg c C-RCV-NXT)
                        (bitwise-or TCP-PSH TCP-ACK) slice #f)
              (cs! c C-SND-NXT (seq+ nxt n))
              (loop)))))
    ;; FIN: after all data is sent, if the owner asked to close, emit it once.
    (if (and (cg c C-FINQ) (not (cg c C-FIN-SENT))
             (= (cg c C-SND-NXT) data-end)
             (seq< (cg c C-SND-NXT) (seq+ (cg c C-SND-UNA) win)))
        (begin
          (cs! c C-FIN-SEQ (cg c C-SND-NXT))
          (tcp-xmit ip mac tx c (cg c C-SND-NXT) (cg c C-RCV-NXT)
                    (bitwise-or TCP-FIN TCP-ACK) #f #f)
          (cs! c C-SND-NXT (seq+ (cg c C-SND-NXT) 1))
          (cs! c C-FIN-SENT #t)))
    (tcp-arm! c now)))

;; --- acknowledgement processing ---------------------------------------------
;; Advance snd-una over newly-acked SYN/data/FIN, trim the send queue, and learn
;; the peer's window. Returns #t if the ack acknowledged our FIN.
(define (tcp-process-ack c ack window now)
  (cs! c C-SND-WND window)
  (if (and (seq< (cg c C-SND-UNA) ack) (seq<= ack (cg c C-SND-NXT)))
      (let ((fin-acked (and (cg c C-FIN-SEQ) (seq< (cg c C-FIN-SEQ) ack))))
        (cs! c C-SND-UNA ack)
        ;; trim acked DATA off the front of sndq (data byte k has seq dbase+k)
        (let ((trim (seq- ack (cg c C-DBASE))))
          (if (and (> trim 0) (<= trim (bytes-length (cg c C-SNDQ))))
              (begin
                (cs! c C-SNDQ (copy-bytes (cg c C-SNDQ) trim
                                          (- (bytes-length (cg c C-SNDQ)) trim)))
                (cs! c C-DBASE (seq+ (cg c C-DBASE) trim)))))
        (tcp-rearm! c now)
        fin-acked)
      #f))

;; --- receive: in-order delivery + out-of-order reassembly -------------------
;; The logical sequence span of a segment: its data PLUS the one number a FIN
;; consumes. (A bare FIN spans one seq, so it is not mistaken for "empty/old".)
(define (seg-span payload fin) (+ (bytes-length payload) (if fin 1 0)))

;; Out-of-order queue: a list of (seq fin . payload), kept sorted by seq. A
;; segment wholly at/below rcv-nxt is dropped; overlaps are NOT resolved here
;; (tcp-deliver trims on delivery), so equal/overlapping seqs may coexist. Bound
;; the queue elsewhere (tcp-accept-segment) so a flood cannot grow it without end.
(define (ooo-insert lst rcv-nxt seq fin payload)
  (if (seq<= (seq+ seq (seg-span payload fin)) rcv-nxt)
      lst                                     ; entirely old: ignore
      (let loop ((l lst))
        (cond ((null? l) (list (cons seq (cons fin payload))))
              ((seq< seq (car (car l)))       ; insert before a later-starting seg
               (cons (cons seq (cons fin payload)) l))
              (else (cons (car l) (loop (cdr l))))))))

;; Deliver as much in-order data to the owner as is now contiguous from rcv-nxt.
;; Each head is handled by its relation to rcv-nxt: a wholly-old head is dropped;
;; a gap stops delivery; a head that starts at-or-before rcv-nxt and extends past
;; it is delivered with its already-received prefix trimmed -- so a retransmitted
;; or overlapping segment neither double-delivers nor leaves an orphan. Returns #t
;; if an in-order FIN was consumed. Mutates c's rcv-nxt and ooo.
(define (tcp-deliver c)
  (let loop ()
    (let ((ooo (cg c C-OOO)))
      (if (null? ooo)
          #f
          (let* ((e (car ooo)) (s (car e)) (fin (car (cdr e))) (payload (cdr (cdr e)))
                 (plen (bytes-length payload))
                 (end (seq+ s (seg-span payload fin)))
                 (rn (cg c C-RCV-NXT)))
            (cond
              ((seq<= end rn) (cs! c C-OOO (cdr ooo)) (loop))   ; wholly old: drop
              ((seq< rn s) #f)                                  ; gap: wait
              (else                                             ; s <= rn < end: deliver
               (cs! c C-OOO (cdr ooo))
               (let ((skip (seq- rn s)))                        ; already-received prefix
                 (if (< skip plen)
                     (let ((chunk (copy-bytes payload skip (- plen skip))))
                       (send (cg c C-OWNER) (list 'tcp-rx (cg c C-ID) chunk))
                       (cs! c C-RCV-NXT (seq+ rn (- plen skip))))))
               (if fin
                   (begin (cs! c C-RCV-NXT (seq+ (cg c C-RCV-NXT) 1)) #t)  ; consume FIN
                   (loop)))))))))

;; A data/FIN-bearing segment for an established-ish connection. `seq` is the
;; sequence number of payload byte 0. Buffer it (in-order data goes through the
;; OOO list too, so one path coalesces), deliver what is now contiguous, then ACK.
;; Returns #t if an in-order FIN landed. Drops segments outside the receive window
;; and caps the queue length so a malicious/lossy peer cannot exhaust the heap.
(define (tcp-accept-segment ip mac tx c seq fin payload)
  (if (and (or (> (bytes-length payload) 0) fin)
           (seq< seq (seq+ (cg c C-RCV-NXT) RCV-WND))    ; within the advertised window
           (< (length (cg c C-OOO)) 64))                 ; bounded reassembly queue
      (cs! c C-OOO (ooo-insert (cg c C-OOO) (cg c C-RCV-NXT) seq fin payload)))
  (let ((finned (tcp-deliver c)))
    ;; cumulative ACK of everything delivered so far
    (tcp-xmit ip mac tx c (cg c C-SND-NXT) (cg c C-RCV-NXT) TCP-ACK #f #f)
    finned))

;; --- the rx state machine ---------------------------------------------------
;; handle-tcp is called from handle-ip with the interface (ip/mac/tx), the TCP
;; service state, the listeners, the frame, the transport offset l4, and len.
(define (handle-tcp ip mac tx tcp frame l4 len src-ip)
  (let ((tlen (- len l4)))
    (if (< tlen 20)
        'ignore
        (let* ((sport (get-be16 frame l4))
               (dport (get-be16 frame (+ l4 2)))
               (seq (get-be32 frame (+ l4 4)))
               (ack (get-be32 frame (+ l4 8)))
               (doff (* (arithmetic-shift (bytes-u8-ref frame (+ l4 12)) -4) 4))
               (flags (bytes-u8-ref frame (+ l4 13)))
               (window (get-be16 frame (+ l4 14)))
               (src-mac (read-mac frame 6)))
          ;; Validate the header length and checksum before trusting any field
          ;; offset (a lying data offset would otherwise drive the payload slice
          ;; or csum past the frame and kill the service context).
          (if (or (< doff 20) (> (+ l4 doff) len)
                  (not (= 0 (csum-seeded frame l4 tlen
                                         (tcp-pseudo-sum src-ip ip tlen)))))
              'ignore
              (let* ((ackf (> (bitwise-and flags TCP-ACK) 0))
                     (synf (> (bitwise-and flags TCP-SYN) 0))
                     (finf (> (bitwise-and flags TCP-FIN) 0))
                     (rstf (> (bitwise-and flags TCP-RST) 0))
                     (paylen (- tlen doff))
                     (payload (if (> paylen 0) (copy-bytes frame (+ l4 doff) paylen)
                                  (make-bytes 0)))
                     (c (tcp-conn-by-tuple tcp dport src-ip sport)))
                (cond
                  ;; test-only injected loss: behave as if the segment never arrived
                  ((tcp-test-drop? tcp synf) 'test-dropped)
                  (c (tcp-rx-conn ip mac tx tcp c synf ackf finf rstf seq ack
                                  window payload))
                  ;; No connection: a SYN to a listening port opens one.
                  ((and synf (not ackf) (not rstf)
                        (hash-has-key? (cg tcp T-LISTEN) dport))
                   (tcp-open-passive ip mac tx tcp dport src-ip sport src-mac seq window))
                  ;; Anything else to a closed port: answer with a RST (unless it
                  ;; is itself a RST), the polite "connection refused".
                  ((not rstf)
                   (tcp-send-rst ip mac tx src-ip src-mac dport sport seq ack
                                 (+ paylen (if synf 1 0) (if finf 1 0)) ackf)
                   'reset)
                  (else 'ignore))))))))

;; A SYN arrived for a listened port: create the half-open connection, send the
;; SYN-ACK, and register it. The owner is the listener's owner; it is told
;; (tcp-accept ...) only once the handshake completes (the final ACK).
(define (tcp-open-passive ip mac tx tcp lport rip rport rmac their-seq window)
  (let* ((id (tcp-next-id tcp))
         (owner (hash-ref (cg tcp T-LISTEN) lport #f))
         (iss (bitwise-and (uptime-ns) #xFFFFFFFF))
         (c (mk-conn id owner lport rip rport rmac iss)))
    (cs! c C-STATE 'syn-rcvd)
    (cs! c C-RCV-NXT (seq+ their-seq 1))      ; consume their SYN
    (cs! c C-SND-WND window)
    (cs! c C-SND-NXT (seq+ iss 1))            ; our SYN consumes one seq
    (tcp-conn-add tcp c)
    (tcp-xmit ip mac tx c iss (cg c C-RCV-NXT) (bitwise-or TCP-SYN TCP-ACK) #f #t)
    (tcp-arm! c (uptime-ns))
    'syn-rcvd))

;; Drive an existing connection on a received segment.
(define (tcp-rx-conn ip mac tx tcp c synf ackf finf rstf seq ack window payload)
  (let ((now (uptime-ns)) (st (cg c C-STATE)))
    (cond
      (rstf (tcp-abort tcp c) 'reset)
      ;; --- active open: waiting for SYN-ACK ---
      ((eq? st 'syn-sent)
       (if (and synf ackf (seq< (cg c C-ISS) ack) (seq<= ack (cg c C-SND-NXT)))
           (begin
             (cs! c C-RCV-NXT (seq+ seq 1)) (cs! c C-SND-UNA ack)
             (cs! c C-SND-WND window) (cs! c C-STATE 'established)
             (tcp-rearm! c now)
             (tcp-xmit ip mac tx c (cg c C-SND-NXT) (cg c C-RCV-NXT) TCP-ACK #f #f)
             (send (cg c C-OWNER) (list 'tcp-connected (cg c C-ID)))
             ;; Data piggybacked on the SYN-ACK starts one past its seq (the SYN
             ;; consumed that number); pass the data's true start seq.
             (if (or (> (bytes-length payload) 0) finf)
                 (tcp-accept-segment ip mac tx c (seq+ seq 1) finf payload))
             (tcp-output ip mac tx c now)
             'established)
           'ignore))
      ;; --- passive open: waiting for the handshake's final ACK ---
      ((eq? st 'syn-rcvd)
       (if (and ackf (seq< (cg c C-SND-UNA) ack) (seq<= ack (cg c C-SND-NXT)))
           (begin
             (tcp-process-ack c ack window now)
             (cs! c C-STATE 'established)
             (send (cg c C-OWNER)
                   (list 'tcp-accept (cg c C-LPORT) (cg c C-ID) (cg c C-RIP) (cg c C-RPORT)))
             (if (or (> (bytes-length payload) 0) finf)
                 (if (tcp-accept-segment ip mac tx c seq finf payload)
                     (cs! c C-STATE 'close-wait)))
             (tcp-output ip mac tx c now)
             'established)
           'ignore))
      ;; --- the connected / closing states ---
      (else
       (if ackf (let ((fin-acked (tcp-process-ack c ack window now)))
                  (tcp-advance-after-ack c fin-acked now)))
       (let ((finned (if (or (> (bytes-length payload) 0) finf)
                         (tcp-accept-segment ip mac tx c seq finf payload)
                         #f)))
         (if finned (tcp-on-peer-fin c now)))
       (tcp-output ip mac tx c now)
       (tcp-maybe-reap tcp c)
       (cg c C-STATE)))))

;; State progression when our FIN gets acknowledged.
(define (tcp-advance-after-ack c fin-acked now)
  (if fin-acked
      (let ((st (cg c C-STATE)))
        (cond ((eq? st 'fin-wait-1) (cs! c C-STATE 'fin-wait-2))
              ((eq? st 'closing) (cs! c C-STATE 'time-wait) (cs! c C-TW (+ now (* 2 MSL-NS))))
              ((eq? st 'last-ack) (cs! c C-STATE 'closed))))))

;; The peer's FIN arrived in order (rcv-nxt already advanced past it).
(define (tcp-on-peer-fin c now)
  (let ((st (cg c C-STATE)))
    (cond
      ((eq? st 'established)
       (cs! c C-STATE 'close-wait)
       (send (cg c C-OWNER) (list 'tcp-closed (cg c C-ID))))
      ((eq? st 'fin-wait-1)            ; simultaneous close
       (cs! c C-STATE 'closing)
       (send (cg c C-OWNER) (list 'tcp-closed (cg c C-ID))))
      ((eq? st 'fin-wait-2)
       (cs! c C-STATE 'time-wait) (cs! c C-TW (+ now (* 2 MSL-NS)))
       (send (cg c C-OWNER) (list 'tcp-closed (cg c C-ID)))))))

;; Abort: tell the owner and drop the connection immediately (RST received).
(define (tcp-abort tcp c)
  (if (not (memq (cg c C-STATE) '(closed time-wait)))
      (send (cg c C-OWNER) (list 'tcp-closed (cg c C-ID))))
  (tcp-conn-del tcp c))

;; Reap a connection that has reached the terminal CLOSED state.
(define (tcp-maybe-reap tcp c)
  (if (eq? (cg c C-STATE) 'closed) (tcp-conn-del tcp c)))

;; --- socket API entry points (called from the service loop) -----------------
(define (tcp-do-listen tcp port owner)
  (hash-set! (cg tcp T-LISTEN) port owner))

(define (tcp-do-connect ip mac tx tcp dst-ip dst-mac dport owner)
  (let* ((id (tcp-next-id tcp))
         (lport (+ 49152 (modulo (uptime-ns) 16000)))   ; ephemeral local port
         (iss (bitwise-and (uptime-ns) #xFFFFFFFF))
         (c (mk-conn id owner lport dst-ip dport dst-mac iss)))
    (cs! c C-STATE 'syn-sent)
    (cs! c C-SND-NXT (seq+ iss 1))            ; SYN consumes one seq
    (tcp-conn-add tcp c)
    (tcp-xmit ip mac tx c iss 0 TCP-SYN #f #t)
    (tcp-arm! c (uptime-ns))
    id))

(define (tcp-do-send ip mac tx tcp id data)
  (let ((c (tcp-conn-by-id tcp id)))
    (if (and c (memq (cg c C-STATE) '(established close-wait)))
        (let* ((old (cg c C-SNDQ)) (ol (bytes-length old)) (dl (bytes-length data))
               (merged (make-bytes (+ ol dl))))
          (bytes-copy-into! merged 0 old ol)
          (bytes-copy-into! merged ol data dl)
          (cs! c C-SNDQ merged)
          (tcp-output ip mac tx c (uptime-ns))))))

(define (tcp-do-close ip mac tx tcp id)
  (let ((c (tcp-conn-by-id tcp id)))
    (if c
        (let ((st (cg c C-STATE)))
          (cs! c C-FINQ #t)
          (cond ((eq? st 'established) (cs! c C-STATE 'fin-wait-1))
                ((eq? st 'close-wait) (cs! c C-STATE 'last-ack)))
          (tcp-output ip mac tx c (uptime-ns))))))

;; --- blocking client helper (runs in the CALLER's context) ------------------
;; A synchronous active open, mirroring arp-resolve: send the connect request
;; with the caller as owner, then block until the handshake completes. Returns
;; the connection handle, or #f if the open was refused/reset. After it returns
;; the caller consumes the connection's (tcp-rx ...) / (tcp-closed ...) events and
;; drives it with (tcp-send ...) / (tcp-close ...) -- the same event API the
;; passive side uses. MUST run in its own context (it blocks on recv).
(define (tcp-connect-blocking net dst-ip dst-mac dport)
  (send net (list 'tcp-connect dst-ip dst-mac dport (self)))
  (let loop ()
    (let ((m (recv)))
      (cond ((eq? (car m) 'tcp-connected) (cadr m))   ; established -> the handle
            ((eq? (car m) 'tcp-closed) #f)            ; refused / reset before open
            (else (loop))))))                          ; ignore anything unrelated

;; --- the periodic tick: retransmission + TIME-WAIT expiry --------------------
(define (tcp-do-tick ip mac tx tcp)
  (let ((now (uptime-ns)))
    (for-each
      (lambda (c)
        (cond
          ;; TIME-WAIT (or a reaped CLOSED) expires and is dropped.
          ((and (> (cg c C-TW) 0) (>= now (cg c C-TW))) (tcp-conn-del tcp c))
          ((eq? (cg c C-STATE) 'closed) (tcp-conn-del tcp c))
          ;; A live retransmit timer fired: back off and resend (Go-Back-N).
          ((and (> (cg c C-RTX) 0) (>= now (cg c C-RTX)))
           (cs! c C-RTO (tcp-min RTO-MAX (* 2 (cg c C-RTO))))
           (tcp-retransmit ip mac tx c now))))
      (hash-values (cg tcp T-ID)))))

;; Resend the oldest unacknowledged data. SYN / SYN-ACK (snd-una == iss) is a
;; control retransmit; otherwise rewind snd-nxt to snd-una and let tcp-output
;; re-emit the data and FIN.
(define (tcp-retransmit ip mac tx c now)
  (if (seq< (cg c C-SND-UNA) (seq+ (cg c C-ISS) 1))
      (cond ((eq? (cg c C-STATE) 'syn-sent)
             (tcp-xmit ip mac tx c (cg c C-ISS) 0 TCP-SYN #f #t))
            ((eq? (cg c C-STATE) 'syn-rcvd)
             (tcp-xmit ip mac tx c (cg c C-ISS) (cg c C-RCV-NXT)
                       (bitwise-or TCP-SYN TCP-ACK) #f #t)))
      (begin
        (cs! c C-SND-NXT (cg c C-SND-UNA))           ; rewind
        (if (and (cg c C-FIN-SEQ) (seq<= (cg c C-SND-UNA) (cg c C-FIN-SEQ)))
            (cs! c C-FIN-SENT #f))                    ; allow tcp-output to resend the FIN
        (tcp-output ip mac tx c now)))
  (cs! c C-RTX (+ now (cg c C-RTO))))                 ; re-arm with the backed-off RTO
