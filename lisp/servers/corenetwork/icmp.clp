;; corenetwork/icmp: ICMP echo (ping).
;; Echo message (8+ bytes): type(1) code(1) csum(2) id(2) seq(2) [data].

(define (build-icmp-echo type id seq)
  (let ((m (make-bytes 8)))
    (bytes-u8-set! m 0 type)          ; 8 = request, 0 = reply
    (bytes-u8-set! m 1 0)             ; code
    (put-be16! m 4 id)
    (put-be16! m 6 seq)
    (put-be16! m 2 (csum m 0 8))      ; checksum over the whole message
    m))

;; ICMP at offset `l4`. Reply to an echo request by flipping the type and
;; recomputing the checksum over the same body. Needs the sender's MAC, which
;; we take from the cache (learned when it ARPed us / we ARPed it).
(define (handle-icmp ip mac nic-tx cache src-ip frame l4 len)
  (let ((ilen (- len l4)))
    ;; only a well-formed echo-request with a valid checksum (as icmp.c required)
    (if (or (< ilen 8) (not (= (u8 frame l4) 8))
            (not (= 0 (csum frame l4 ilen))))
        'ignore
        (let ((dst-mac (cache-get cache src-ip (uptime-ns))))
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
