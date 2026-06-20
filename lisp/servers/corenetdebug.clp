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

  (define ECHO-PORT   1337)
  (define UPLOAD-PORT 1338)

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
      (display "[corenetdebug] enabled (echo 1337, digest 1338)") (newline)
      'netdbg-up)))
