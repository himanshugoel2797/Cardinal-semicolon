;; corenetwork/common: shared constants and small byte/field readers. Included
;; into the corenetwork module's scope (not a module of its own).

(define (u8 b i) (bytes-u8-ref b i))

(define ETH-ARP  #x0806)
(define ETH-IPV4 #x0800)
(define IP-ICMP 1)
(define IP-UDP 17)

(define (read-mac b off)
  (list (u8 b off) (u8 b (+ off 1)) (u8 b (+ off 2))
        (u8 b (+ off 3)) (u8 b (+ off 4)) (u8 b (+ off 5))))
(define (read-ip b off)
  (list (u8 b off) (u8 b (+ off 1)) (u8 b (+ off 2)) (u8 b (+ off 3))))

(define BROADCAST (list #xFF #xFF #xFF #xFF #xFF #xFF))
;; The IPv4 limited broadcast address (255.255.255.255). Accepted on rx in
;; addition to our own unicast address -- the DHCP path receives OFFER/ACK here
;; (the server broadcasts the reply while our interface is still 0.0.0.0).
(define IP-BROADCAST (list #xFF #xFF #xFF #xFF))
;; The unconfigured interface address (0.0.0.0): DHCP brings the interface up
;; here and the address layer is set from the lease.
(define IP-ANY (list 0 0 0 0))

;; Pack a 4-byte IP list into a fixnum so it is an eq?-comparable cache key.
(define (ip->key ip)
  (+ (arithmetic-shift (car ip) 24) (arithmetic-shift (cadr ip) 16)
     (arithmetic-shift (caddr ip) 8) (cadddr ip)))

;; Two bytes of a 4-byte IP list as a big-endian 16-bit word (for pseudo-headers).
(define (get-be16-list l off)
  (+ (arithmetic-shift (nth l off) 8) (nth l (+ off 1))))
