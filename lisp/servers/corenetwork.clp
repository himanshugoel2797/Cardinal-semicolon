;; corenetwork: the IPv4 network stack, ported from servers/CoreNetwork
;; (ethernet/arp/ip/icmp/udp). One long-lived service context owns the interface
;; (our IP + MAC), the ARP cache, and the UDP port table; it receives raw frames
;; from a NIC driver and demuxes ethernet -> ARP / IPv4 -> ICMP / UDP, and builds
;; replies it hands back to the NIC for transmission.
;;
;; The C server's synchronous call chain (ethernet_rx -> arp_rx -> ethernet_tx,
;; and the udp handler calling back into udp_send_to) is exactly the rx-handler-
;; re-enters-tx pattern that forced the "copy the handler out from under the lock"
;; dance there. Here every step is a message: the NIC sends (rx frame len), the
;; service sends the NIC (tx frame len), a UDP handler is a separate context the
;; service sends datagrams to -- no re-entrancy, no locks.
;;
;; Wire formats are big-endian; put-be16!/get-be16 (driver-util) do the byte
;; assembly. The internet checksum (RFC 1071) is computed over big-endian 16-bit
;; words -- byte-for-byte identical on the wire to the C's native-order version
;; (the one's-complement sum commutes with the per-word byte swap).
;;
;; This module is ONE capability, but its source is split across the files in
;; ./corenetwork/<part>.clp -- per protocol layer -- and spliced into this scope
;; by `include`. The parts are not modules; they share these private definitions
;; and none is importable on its own (a component's internals stay internal).
;;
;; Service protocol (send these to the handle from start-network-service):
;;   (register-nic <mac-list> <tx-ctx>)        ; the NIC announces itself
;;   (rx <frame-bytes> <len>)                  ; a received ethernet frame
;;   (arp-request <ip-list>)                   ; emit an ARP who-has
;;   (arp-lookup <ip-list> <reply-ctx>)        ; reply (mac | #f) from the cache
;;   (udp-bind <port> <handler-ctx>)           ; datagrams to <port> -> handler
;;   (udp-send <dst-ip> <dst-mac> <sport> <dport> <payload-bytes>)
;;   (ping <dst-ip> <dst-mac> <id> <seq>)      ; emit an ICMP echo request
;;   (set-address <ip> <netmask> <gateway> <dns>) ; write the interface config
;;   (get-address <reply-ctx>)                 ; reply (ip netmask gateway dns)
;;   (dhcp-start)                              ; begin DHCP on this interface
;; A bound UDP handler receives (udp-rx <src-ip> <src-mac> <src-port> <payload>).
;;
;; TCP socket protocol (messages to the same service handle):
;;   (tcp-listen <port> <owner>)                ; passive open on <port>
;;   (tcp-connect <dst-ip> <dst-mac> <dport> <owner>) ; active open
;;   (tcp-send <conn> <payload-bytes>)          ; queue data on a connection
;;   (tcp-close <conn>)                         ; begin the active close (send FIN)
;; The <owner> context receives, per connection:
;;   (tcp-accept <lport> <conn> <rip> <rport>)  ; an inbound connection established
;;   (tcp-connected <conn>)                     ; our active open completed
;;   (tcp-rx <conn> <payload-bytes>)            ; in-order received data
;;   (tcp-closed <conn>)                        ; peer closed (or the conn aborted)
;;
;; Also exported: arp-resolve (synchronous outbound next-hop resolution),
;; tcp-connect-blocking (a synchronous active-open helper, like arp-resolve, that
;; returns a connection handle once the handshake completes), and dns-resolve
;; (synchronous A-record lookup against the DHCP-learned DNS server). All three
;; block and must run in their own context.

(define-module corenetwork
  (export start-network-service arp-resolve tcp-connect-blocking dns-resolve)
  (import driver-util)
  ;; layer by layer: shared helpers, the checksum, then eth/arp/ip/icmp/udp, the
  ;; DHCP client, then the service loop that ties them together.
  (include common route firewall checksum eth arp ip icmp udp tcp dhcp dns service))
