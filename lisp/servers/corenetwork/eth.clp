;; corenetwork/eth: ethernet TX. Build a frame around `payload` (plen bytes) and
;; send it to the NIC. Frames shorter than 60 bytes are zero-padded (make-bytes
;; zero-fills), so a NIC that does not pad still emits a legal frame.

(define (eth-tx nic-tx src-mac dst-mac ethertype payload plen)
  (let ((flen (+ 14 plen)))
    (let ((f (make-bytes (if (< flen 60) 60 flen))))
      (put-list! f 0 dst-mac)
      (put-list! f 6 src-mac)
      (put-be16! f 12 ethertype)
      (bytes-copy-into! f 14 payload plen)
      (send nic-tx (list 'tx f (bytes-length f))))))
