;; corenetwork/service: the long-lived network service loop -- the one exported
;; entry point. State is (ip mac nic-tx arp-cache udp-binds netmask gateway dns
;; tcp); `serve` threads it. ip/netmask/gateway/dns are the interface's address
;; config (set statically at start, or learned by the DHCP client and written
;; back via the set-address message). `tcp` is the mutable TCP state (connection
;; tables + listeners); its contents are mutated in place, so the same object
;; rides through every state transition.

;; State constructor, so the 9-tuple is built in one place.
(define (mk-state ip mac tx cache binds nm gw dns tcp)
  (list ip mac tx cache binds nm gw dns tcp))

;; A tiny ticker context: every TICK-MS it nudges the service to run TCP timers
;; (the service itself can never block/sleep). It receives nothing, so a stray
;; send can't wake it early.
(define (start-tcp-ticker net)
  (spawn-restricted '()
    (lambda ()
      (let loop () (sleep (* TICK-MS 1000000)) (send net (list 'tcp-tick)) (loop)))))

(define (start-network-service our-ip)
  (let* ((tcp0 (mk-tcp-state))
         (net
    (serve (mk-state our-ip #f #f '() '() IP-ANY IP-ANY IP-ANY tcp0)
    (lambda (st m)
      (let ((ip (nth st 0)) (mac (nth st 1)) (tx (nth st 2))
            (cache (nth st 3)) (binds (nth st 4))
            (nm (nth st 5)) (gw (nth st 6)) (dns (nth st 7)) (tcp (nth st 8)))
        (cond
          ((eq? (car m) 'register-nic)         ; (register-nic mac tx-ctx)
           (display "[corenetwork] nic registered, mac=")
           (display (cadr m)) (newline)
           (mk-state ip (cadr m) (caddr m) cache binds nm gw dns tcp))
          ((eq? (car m) 'rx)                   ; (rx frame len)
           (let ((frame (cadr m)) (len (caddr m)))
             (if (< len 14)
                 st
                 (let ((etype (get-be16 frame 12)))
                   (cond
                     ((= etype ETH-ARP)
                      (mk-state ip mac tx
                                (handle-arp ip mac tx cache (uptime-ns) frame len)
                                binds nm gw dns tcp))
                     ((= etype ETH-IPV4)
                      (handle-ip ip mac tx cache binds tcp frame len)
                      st)
                     (else st))))))
          ((eq? (car m) 'arp-request)          ; (arp-request ip)
           (if (and mac tx)
               (eth-tx tx mac BROADCAST ETH-ARP
                       (build-arp 1 mac ip (list 0 0 0 0 0 0) (cadr m)) 28))
           st)
          ((eq? (car m) 'arp-lookup)           ; (arp-lookup ip reply)
           (send (caddr m) (cache-get cache (cadr m) (uptime-ns)))
           st)
          ((eq? (car m) 'udp-bind)             ; (udp-bind port handler)
           (display "[corenetwork] udp port bound: ")
           (display (cadr m)) (newline)
           (mk-state ip mac tx cache (cons (cons (cadr m) (caddr m)) binds)
                     nm gw dns tcp))
          ((eq? (car m) 'udp-unbind)           ; (udp-unbind port) -- drop the binding
           (mk-state ip mac tx cache
                     (filter (lambda (b) (not (= (car b) (cadr m)))) binds)
                     nm gw dns tcp))
          ((eq? (car m) 'udp-send)             ; (udp-send dst-ip dst-mac sport dport payload)
           (if (and mac tx)
               (udp-send ip mac tx (cadr m) (caddr m) (cadddr m)
                         (nth m 4) (nth m 5) (bytes-length (nth m 5))))
           st)
          ((eq? (car m) 'ping)                 ; (ping dst-ip dst-mac id seq)
           (if (and mac tx)
               (eth-tx tx mac (caddr m) ETH-IPV4
                       (build-ipv4 ip (cadr m) IP-ICMP
                                   (build-icmp-echo 8 (cadddr m) (nth m 4)) 8)
                       28))
           st)
          ((eq? (car m) 'set-address)          ; (set-address ip nm gw dns)
           (display "[corenetwork] address set ") (display (cadr m)) (newline)
           (mk-state (cadr m) mac tx cache binds (caddr m) (cadddr m) (nth m 4) tcp))
          ((eq? (car m) 'get-address)          ; (get-address reply) -> (ip nm gw dns)
           (send (cadr m) (list ip nm gw dns))
           st)
          ((eq? (car m) 'dhcp-start)           ; (dhcp-start) -- begin DHCP on this iface
           (if (and mac tx)
               (let ((net (self)) (hw mac))
                 (spawn-restricted '() (lambda () (dhcp-client net hw)))))
           st)
          ;; --- TCP socket API (state mutated in place; the same st rides on) ---
          ((eq? (car m) 'tcp-listen)           ; (tcp-listen port owner)
           (tcp-do-listen tcp (cadr m) (caddr m))
           (display "[corenetwork] tcp listening on ") (display (cadr m)) (newline)
           st)
          ((eq? (car m) 'tcp-connect)          ; (tcp-connect dst-ip dst-mac dport owner)
           (if (and mac tx)
               (tcp-do-connect ip mac tx tcp (cadr m) (caddr m) (cadddr m) (nth m 4)))
           st)
          ((eq? (car m) 'tcp-send)             ; (tcp-send conn bytes)
           (if (and mac tx) (tcp-do-send ip mac tx tcp (cadr m) (caddr m)))
           st)
          ((eq? (car m) 'tcp-close)            ; (tcp-close conn)
           (if (and mac tx) (tcp-do-close ip mac tx tcp (cadr m)))
           st)
          ((eq? (car m) 'tcp-tick)             ; (tcp-tick) -- from the ticker context
           (if (and mac tx) (tcp-do-tick ip mac tx tcp))
           st)
          ((eq? (car m) 'tcp-test-loss)        ; (tcp-test-loss N) -- test fault injection
           (tcp-do-test-loss tcp (cadr m))
           (display "[corenetwork] tcp test-loss: drop 1 in ")
           (display (cadr m)) (newline)
           st)
          (else st)))))))
    (start-tcp-ticker net)
    net))
