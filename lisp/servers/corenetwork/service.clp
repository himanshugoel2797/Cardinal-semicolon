;; corenetwork/service: the long-lived network service loop -- the one exported
;; entry point. State is (ip mac nic-tx arp-cache udp-binds netmask gateway dns);
;; `serve` threads it. ip/netmask/gateway/dns are the interface's address config
;; (set statically at start, or learned by the DHCP client and written back via
;; the set-address message).

;; State constructor, so the 8-tuple is built in one place.
(define (mk-state ip mac tx cache binds nm gw dns)
  (list ip mac tx cache binds nm gw dns))

(define (start-network-service our-ip)
  (serve (mk-state our-ip #f #f '() '() IP-ANY IP-ANY IP-ANY)
    (lambda (st m)
      (let ((ip (nth st 0)) (mac (nth st 1)) (tx (nth st 2))
            (cache (nth st 3)) (binds (nth st 4))
            (nm (nth st 5)) (gw (nth st 6)) (dns (nth st 7)))
        (cond
          ((eq? (car m) 'register-nic)         ; (register-nic mac tx-ctx)
           (display "[corenetwork] nic registered, mac=")
           (display (cadr m)) (newline)
           (mk-state ip (cadr m) (caddr m) cache binds nm gw dns))
          ((eq? (car m) 'rx)                   ; (rx frame len)
           (let ((frame (cadr m)) (len (caddr m)))
             (if (< len 14)
                 st
                 (let ((etype (get-be16 frame 12)))
                   (cond
                     ((= etype ETH-ARP)
                      (mk-state ip mac tx
                                (handle-arp ip mac tx cache (uptime-ns) frame len)
                                binds nm gw dns))
                     ((= etype ETH-IPV4)
                      (handle-ip ip mac tx cache binds frame len)
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
                     nm gw dns))
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
           (mk-state (cadr m) mac tx cache binds (caddr m) (cadddr m) (nth m 4)))
          ((eq? (car m) 'get-address)          ; (get-address reply) -> (ip nm gw dns)
           (send (cadr m) (list ip nm gw dns))
           st)
          ((eq? (car m) 'dhcp-start)           ; (dhcp-start) -- begin DHCP on this iface
           (if (and mac tx)
               (let ((net (self)) (hw mac))
                 (spawn-restricted '() (lambda () (dhcp-client net hw)))))
           st)
          (else st))))))
