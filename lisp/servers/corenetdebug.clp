;; corenetdebug: an optional, cmdline-gated network debug endpoint, ported from
;; servers/CoreNetDebug. Built on the Lisp CoreNetwork UDP facility -- a swap-free,
;; far-faster-than-serial way to poke the running OS:
;;
;;   UDP 1337 (echo)   : every datagram is bounced straight back to the sender
;;                       (responder-driven -- we reply to the captured src-mac, so
;;                       no ARP resolution is needed). Liveness + round-trip latency.
;;   UDP 1338 (digest) : FNV-1a digest a received datagram to the console so a host
;;                       tool can confirm a blob arrived intact. This is a SINGLE-
;;                       datagram stand-in for the C's RDT reliable named-blob
;;                       upload: the Lisp CoreNetwork has UDP but not yet RDT, so
;;                       the reliable multi-packet path is a follow-up.
;;
;; Inert unless init starts it (init gates this on "cardinal.netdbg" -- a remote
;; attack surface, opt-in just like the serial debug shell). Each endpoint is its
;; own restricted handler context that the UDP layer delivers (udp-rx ...) to.

(define-module corenetdebug
  (export start-netdebug)

  (define ECHO-PORT     1337)
  (define UPLOAD-PORT   1338)
  (define TCP-ECHO-PORT 7)      ; classic TCP "echo" service port

  ;; FNV-1a over a bytes buffer, 32-bit (xor then multiply, masked to 32 bits) --
  ;; an order-sensitive digest, matching the C, so the host confirms integrity not
  ;; just length.
  (define (fnv1a b)
    (let ((n (bytes-length b)))
      (let loop ((i 0) (h 2166136261))
        (if (>= i n)
            h
            (loop (+ i 1)
                  (bitwise-and (* (bitwise-xor h (bytes-u8-ref b i)) 16777619)
                               #xFFFFFFFF))))))

  ;; Spawn the two handler contexts and bind them to their ports. `net` is the
  ;; CoreNetwork service handle; a bound handler receives
  ;;   (udp-rx <src-ip> <src-mac> <src-port> <payload-bytes>).
  (define (start-netdebug net)
    (let ((echo
            (spawn-restricted '()
              (lambda ()
                (let loop ()
                  (let ((m (recv)))
                    (if (eq? (car m) 'udp-rx)
                        (send net (list 'udp-send (cadr m) (caddr m)
                                        ECHO-PORT (cadddr m) (nth m 4))))
                    (loop))))))
          (upload
            (spawn-restricted '()
              (lambda ()
                (let loop ()
                  (let ((m (recv)))
                    (if (eq? (car m) 'udp-rx)
                        (let ((payload (nth m 4)))
                          (display "[corenetdebug] upload len=")
                          (display (bytes-length payload))
                          (display " digest=") (display (fnv1a payload))
                          (newline)))
                    (loop)))))))
      (send net (list 'udp-bind ECHO-PORT echo))
      (send net (list 'udp-bind UPLOAD-PORT upload))
      (start-tcp-echo net)
      (display "[corenetdebug] enabled (udp echo 1337, digest 1338, tcp echo 7)") (newline)
      'netdbg-up))

  ;; A TCP echo server on port 7: a stream consumer of the CoreNetwork socket API.
  ;; It listens, and per connection bounces every received chunk straight back
  ;; (tcp-send) and, when the peer half-closes, closes its own side. This is the
  ;; reliable, connection-oriented analogue of the UDP echo above -- exercising the
  ;; handshake, in-order data, retransmission, and FIN teardown end to end.
  (define (start-tcp-echo net)
    (let ((srv
            (spawn-restricted '()
              (lambda ()
                (let loop ()
                  (let ((m (recv)))
                    (cond
                      ((eq? (car m) 'tcp-accept)        ; (tcp-accept lport conn rip rport)
                       (display "[corenetdebug] tcp connection accepted (conn ")
                       (display (caddr m)) (display ")") (newline))
                      ((eq? (car m) 'tcp-rx)            ; (tcp-rx conn bytes) -> echo it back
                       (send net (list 'tcp-send (cadr m) (caddr m))))
                      ((eq? (car m) 'tcp-closed)        ; (tcp-closed conn) -> close our side
                       (send net (list 'tcp-close (cadr m)))))
                    (loop)))))))
      (send net (list 'tcp-listen TCP-ECHO-PORT srv)))))
