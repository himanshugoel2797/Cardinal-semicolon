;; corenetwork/service: the long-lived network service loop -- the one exported
;; entry point. State is (ifaces routes arp-cache udp-binds tcp); `serve` threads
;; it. `ifaces` is the list of network interfaces (each a mutable record with its
;; own mac/tx and address config -- see route.clp); `routes` is the IPv4 routing
;; table. The ARP cache, UDP port binds, and the TCP connection table are global
;; (a socket/connection is not tied to one NIC; routing picks the egress per
;; packet). `tcp` is mutated in place, so the same object rides through.
;;
;; Multi-homed HOST (no forwarding): the egress for a reply is the interface that
;; owns the IP the peer addressed; the egress for an active send is chosen by the
;; routing table (route-egress). Interfaces are created by register-nic and
;; addressed by set-address (which also installs the interface's routes).

;; State constructor, so the 5-tuple is built in one place.
(define (mk-state ifaces routes cache binds tcp)
  (list ifaces routes cache binds tcp))

;; The egress interface for a destination, or #f if unreachable. The limited
;; broadcast (255.255.255.255) goes out the primary interface regardless of
;; routes -- this is the path DHCP uses to send DISCOVER/REQUEST before the
;; interface has any address or route yet.
(define (egress-for ifaces routes dst)
  (if (equal? dst IP-BROADCAST)
      (primary-iface-any ifaces)
      (let ((e (route-egress routes dst))) (and e (car e)))))

;; A tiny ticker context: every TICK-MS it nudges the service to run TCP timers
;; (the service itself can never block/sleep). It receives nothing, so a stray
;; send can't wake it early.
(define (start-tcp-ticker net)
  (spawn-restricted '()
    (lambda ()
      (let loop () (sleep (* TICK-MS 1000000)) (send net (list 'tcp-tick)) (loop)))))

(define (start-network-service)
  (let* ((tcp0 (mk-tcp-state))
         (fw (mk-fw))               ; inbound packet filter (closure state, mutated by fw-* msgs)
         (txq (mk-txq))             ; async TX hold-queue (pending ARP resolutions)
         (reasm (mk-reasm))         ; inbound IPv4 fragment reassembly buffer
         (net
    (serve (mk-state '() '() '() '() tcp0)
    (lambda (st m)
      (let ((ifaces (nth st 0)) (routes (nth st 1)) (cache (nth st 2))
            (binds (nth st 3)) (tcp (nth st 4)))
        (cond
          ((eq? (car m) 'register-nic)         ; (register-nic mac tx-ctx)
           (display "[corenetwork] nic registered as if") (display (length ifaces))
           (display ", mac=") (display (cadr m)) (newline)
           (mk-state (append ifaces (list (mk-iface (length ifaces) (cadr m) (caddr m))))
                     routes cache binds tcp))
          ((eq? (car m) 'rx)                   ; (rx frame len)
           (let ((frame (cadr m)) (len (caddr m)))
             (if (< len 14)
                 st
                 (let ((etype (get-be16 frame 12)))
                   (cond
                     ((= etype ETH-ARP)
                      ;; Learn from the ARP, then flush any held packets whose
                      ;; next hop just resolved (the async TX path).
                      (let ((cache2 (handle-arp ifaces cache (uptime-ns) frame len)))
                        (txq-flush! txq cache2 (uptime-ns))
                        (mk-state ifaces routes cache2 binds tcp)))
                     ((= etype ETH-IPV4)
                      (handle-ip ifaces routes fw reasm cache binds tcp frame len)
                      st)
                     (else st))))))
          ((eq? (car m) 'arp-request)          ; (arp-request ip) -- who-has on the route's link
           (let ((i (egress-for ifaces routes (cadr m))))
             (if i
                 (eth-tx (ig i I-TX) (ig i I-MAC) BROADCAST ETH-ARP
                         (build-arp 1 (ig i I-MAC) (ig i I-IP) (list 0 0 0 0 0 0) (cadr m)) 28)))
           st)
          ((eq? (car m) 'arp-lookup)           ; (arp-lookup ip reply)
           (send (caddr m) (cache-get cache (cadr m) (uptime-ns)))
           st)
          ((eq? (car m) 'udp-bind)             ; (udp-bind port handler)
           (display "[corenetwork] udp port bound: ")
           (display (cadr m)) (newline)
           (mk-state ifaces routes cache (cons (cons (cadr m) (caddr m)) binds) tcp))
          ((eq? (car m) 'udp-unbind)           ; (udp-unbind port) -- drop the binding
           (mk-state ifaces routes cache
                     (filter (lambda (b) (not (= (car b) (cadr m)))) binds) tcp))
          ((eq? (car m) 'udp-send)             ; (udp-send dst-ip dst-mac sport dport payload)
           (let ((i (egress-for ifaces routes (cadr m))))
             (if i
                 (udp-send (ig i I-IP) (ig i I-MAC) (ig i I-TX) (cadr m) (caddr m) (cadddr m)
                           (nth m 4) (nth m 5) (bytes-length (nth m 5)))))
           st)
          ((eq? (car m) 'udp-send-if)          ; (udp-send-if mac dst-ip dst-mac sport dport payload)
           (let ((i (iface-by-mac ifaces (cadr m))))  ; egress on a SPECIFIC interface (DHCP)
             (if i
                 (udp-send (ig i I-IP) (ig i I-MAC) (ig i I-TX) (caddr m) (cadddr m) (nth m 4)
                           (nth m 5) (nth m 6) (bytes-length (nth m 6)))))
           st)
          ((eq? (car m) 'udp-send-async)       ; (udp-send-async dst-ip sport dport payload)
           ;; Fire-and-forget: route + resolve the next-hop MAC ourselves, holding
           ;; the datagram (and ARPing) on a cache miss -- no caller-side arp-resolve.
           (let ((e (route-egress routes (cadr m))))
             (if e
                 (let* ((iface (car e)) (nh (cdr e)) (pl (nth m 4)) (plen (bytes-length pl)))
                   (txq-send! txq cache (uptime-ns) iface nh ETH-IPV4
                              (udp-build-ip (ig iface I-IP) (cadr m) (caddr m) (cadddr m) pl plen)
                              (+ 28 plen)))))
           st)
          ((eq? (car m) 'ping)                 ; (ping dst-ip dst-mac id seq)
           (let ((i (egress-for ifaces routes (cadr m))))
             (if i
                 (eth-tx-ip (ig i I-TX) (ig i I-MAC) (caddr m)
                            (build-ipv4 (ig i I-IP) (cadr m) IP-ICMP
                                        (build-icmp-echo 8 (cadddr m) (nth m 4)) 8)
                            28)))
           st)
          ((eq? (car m) 'set-address)          ; (set-address mac ip nm gw dns) -- mac #f = first iface
           (let ((i (if (cadr m) (iface-by-mac ifaces (cadr m)) (primary-iface-any ifaces))))
             (if (not i)
                 st
                 (begin
                   (is! i I-IP (caddr m)) (is! i I-MASK (cadddr m))
                   (is! i I-GW (nth m 4)) (is! i I-DNS (nth m 5))
                   (display "[corenetwork] if") (display (ig i I-ID))
                   (display " address set ") (display (caddr m)) (newline)
                   (mk-state ifaces (routes-install routes i) cache binds tcp)))))
          ((eq? (car m) 'get-address)          ; (get-address reply) -> (ip nm gw dns) of primary
           (let ((i (primary-iface ifaces)))
             (send (cadr m) (if i (list (ig i I-IP) (ig i I-MASK) (ig i I-GW) (ig i I-DNS))
                                (list IP-ANY IP-ANY IP-ANY IP-ANY))))
           st)
          ((eq? (car m) 'route-query)          ; (route-query dst reply) -> (src-ip mac tx next-hop)|#f
           (let ((e (route-egress routes (cadr m))))
             (send (caddr m)
                   (if e (list (ig (car e) I-IP) (ig (car e) I-MAC) (ig (car e) I-TX) (cdr e))
                       #f)))
           st)
          ((eq? (car m) 'dhcp-start)           ; (dhcp-start) -- DHCP on the primary interface
           (let ((i (primary-iface-any ifaces)))
             (if i
                 (let ((nn (self)) (hw (ig i I-MAC)))
                   (spawn-restricted '() (lambda () (dhcp-client nn hw))))))
           st)
          ((eq? (car m) 'dhcp-start-all)       ; (dhcp-start-all) -- DHCP on every interface
           (for-each (lambda (i)
                       (let ((nn (self)) (hw (ig i I-MAC)))
                         (spawn-restricted '() (lambda () (dhcp-client nn hw)))))
                     ifaces)
           st)
          ;; --- TCP socket API (state mutated in place; the same st rides on) ---
          ((eq? (car m) 'tcp-listen)           ; (tcp-listen port owner)
           (tcp-do-listen tcp (cadr m) (caddr m))
           (display "[corenetwork] tcp listening on ") (display (cadr m)) (newline)
           st)
          ((eq? (car m) 'tcp-connect)          ; (tcp-connect dst-ip dst-mac dport owner)
           (let ((i (egress-for ifaces routes (cadr m))))
             (if i
                 (tcp-do-connect (ig i I-IP) (ig i I-MAC) (ig i I-TX) tcp
                                 (cadr m) (caddr m) (cadddr m) (nth m 4))))
           st)
          ((eq? (car m) 'tcp-send)             ; (tcp-send conn bytes)
           (tcp-do-send tcp (cadr m) (caddr m))
           st)
          ((eq? (car m) 'tcp-close)            ; (tcp-close conn)
           (tcp-do-close tcp (cadr m))
           st)
          ((eq? (car m) 'tcp-tick)             ; (tcp-tick) -- periodic timer from the ticker
           (tcp-do-tick tcp)
           (txq-tick! txq (uptime-ns))         ; re-ARP / time-out held packets too
           (reasm-tick! reasm (uptime-ns))     ; time-out incomplete reassemblies
           st)
          ((eq? (car m) 'tcp-test-loss)        ; (tcp-test-loss N) -- test fault injection
           (tcp-do-test-loss tcp (cadr m))
           (display "[corenetwork] tcp test-loss: drop 1 in ")
           (display (cadr m)) (newline)
           st)
          ;; --- inbound firewall config (fw mutated in place) ---
          ((eq? (car m) 'fw-policy)            ; (fw-policy allow|deny) -- default action
           (fw-set-policy! fw (eq? (cadr m) 'allow))
           (display "[corenetwork] firewall default ") (display (cadr m)) (newline)
           st)
          ((eq? (car m) 'fw-add)               ; (fw-add action proto src-net src-len dport)
           (fw-add! fw (fw-rule (cadr m) (caddr m) (cadddr m) (nth m 4) (nth m 5)))
           (display "[corenetwork] firewall rule added") (newline)
           st)
          ((eq? (car m) 'fw-clear)             ; (fw-clear) -- drop all rules
           (fw-clear! fw) st)
          ((eq? (car m) 'fw-query)             ; (fw-query proto src-ip dport reply) -> #t|#f
           (send (nth m 4) (fw-allow? fw (cadr m) (caddr m) (cadddr m)))
           st)
          (else st)))))))
    (start-tcp-ticker net)
    net))
