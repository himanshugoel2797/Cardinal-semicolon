;; corenetwork/route: interface records + an IPv4 routing table. This is what
;; makes the stack multi-homed: each NIC becomes an interface with its own
;; address config, and a longest-prefix-match routing table decides, for any
;; destination, which interface to send out and what the next hop is.
;;
;; Multi-homed HOST only (no forwarding): we never relay between interfaces, we
;; just pick our own egress. The egress for a REPLY is simply the interface that
;; owns the IP the peer addressed (iface-for-ip); the egress for an ACTIVE send is
;; chosen by route-egress (longest-prefix match, gateway or on-link). No ingress
;; tagging is needed, so the NIC drivers are untouched.

;; --- interface record (a mutable vector) ------------------------------------
(define I-ID 0) (define I-MAC 1) (define I-TX 2)
(define I-IP 3) (define I-MASK 4) (define I-GW 5) (define I-DNS 6) (define I-LEN 7)
(define (ig i k) (vector-ref i k))
(define (is! i k v) (vector-set! i k v))
(define (mk-iface id mac tx)
  (let ((i (make-vector I-LEN IP-ANY)))
    (is! i I-ID id) (is! i I-MAC mac) (is! i I-TX tx) i))

;; The interface that owns `ip` (a configured unicast address), or #f. Used to
;; pick the egress for a reply: we answer from the address the peer targeted.
(define (iface-for-ip ifaces ip)
  (cond ((null? ifaces) #f)
        ((equal? (ig (car ifaces) I-IP) ip) (car ifaces))
        (else (iface-for-ip (cdr ifaces) ip))))

(define (iface-by-mac ifaces mac)
  (cond ((null? ifaces) #f)
        ((equal? (ig (car ifaces) I-MAC) mac) (car ifaces))
        (else (iface-by-mac (cdr ifaces) mac))))

;; The first configured interface (non-zero IP), or the first interface, or #f --
;; the "primary", used where a single default is wanted (e.g. get-address).
(define (primary-iface ifaces)
  (cond ((null? ifaces) #f)
        ((not (equal? (ig (car ifaces) I-IP) IP-ANY)) (car ifaces))
        (else (primary-iface (cdr ifaces)))))

;; The first interface regardless of whether it has an address yet -- used by
;; DHCP, which by definition runs on an interface still at 0.0.0.0.
(define (primary-iface-any ifaces) (if (null? ifaces) #f (car ifaces)))

;; --- routing table ----------------------------------------------------------
;; A route is (network masklen gateway iface): `gateway` is #f for an on-link
;; (directly-connected) route, else the next-hop IP; `iface` is the egress
;; interface record. Longest-prefix match wins.
(define (mk-route network masklen gw iface) (list network masklen gw iface))
(define (route-net r) (car r))
(define (route-masklen r) (cadr r))
(define (route-gw r) (caddr r))
(define (route-iface r) (cadddr r))

;; Do the first `masklen` bits of IPs a and b agree? (a/b are 4-element lists.)
(define (ip-prefix-eq? a b masklen)
  (let loop ((i 0) (bits masklen))
    (cond ((<= bits 0) #t)
          ((>= bits 8) (and (= (nth a i) (nth b i)) (loop (+ i 1) (- bits 8))))
          (else (let ((m (bitwise-and (arithmetic-shift #xFF (- 8 bits)) #xFF)))
                  (= (bitwise-and (nth a i) m) (bitwise-and (nth b i) m)))))))

;; Longest-prefix-match route for `dst`, or #f. A 0-length default route matches
;; everything (prefix-eq? with 0 bits is true), so it is the natural fallback.
(define (route-lookup routes dst)
  (let loop ((rs routes) (best #f))
    (cond ((null? rs) best)
          ((and (ip-prefix-eq? (route-net (car rs)) dst (route-masklen (car rs)))
                (or (not best) (> (route-masklen (car rs)) (route-masklen best))))
           (loop (cdr rs) (car rs)))
          (else (loop (cdr rs) best)))))

;; Resolve `dst` to (iface . next-hop-ip), or #f if unreachable. On an on-link
;; route the next hop IS the destination; otherwise it is the route's gateway.
(define (route-egress routes dst)
  (let ((r (route-lookup routes dst)))
    (if (not r)
        #f
        (cons (route-iface r) (if (route-gw r) (route-gw r) dst)))))

;; Population: when an interface gets an address (DHCP/static), install its
;; on-link route (network/masklen via that interface, direct) and, if a gateway
;; was learned, a default route (0.0.0.0/0 via the gateway). Existing routes
;; through that interface are dropped first so re-configuration is idempotent.
(define (netmask->len mask)
  (let loop ((l mask) (n 0))
    (if (null? l) n (loop (cdr l) (+ n (popcount8 (car l)))))))
(define (popcount8 b)
  (let loop ((b b) (n 0)) (if (= b 0) n (loop (arithmetic-shift b -1) (+ n (bitwise-and b 1))))))
(define (ip-network ip masklen)               ; ip AND the prefix mask
  (let loop ((i 0) (bits masklen) (out '()))
    (cond ((= i 4) (reverse out))
          ((>= bits 8) (loop (+ i 1) (- bits 8) (cons (nth ip i) out)))
          (else (let ((m (bitwise-and (arithmetic-shift #xFF (- 8 (if (< bits 0) 0 bits))) #xFF)))
                  (loop (+ i 1) 0 (cons (bitwise-and (nth ip i) m) out)))))))

(define (routes-without-iface routes iface)
  (filter (lambda (r) (not (eq? (route-iface r) iface))) routes))

;; Returns the new route list after (re)installing iface's on-link + default routes.
(define (routes-install routes iface)
  (let* ((ip (ig iface I-IP)) (mask (ig iface I-MASK)) (gw (ig iface I-GW))
         (mlen (netmask->len mask))
         (base (routes-without-iface routes iface))
         (with-onlink (cons (mk-route (ip-network ip mlen) mlen #f iface) base)))
    (if (equal? gw IP-ANY)
        with-onlink
        (cons (mk-route (list 0 0 0 0) 0 gw iface) with-onlink))))   ; default route
