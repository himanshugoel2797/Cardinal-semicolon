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
;; A bound UDP handler receives (udp-rx <src-ip> <src-mac> <src-port> <payload>).

(define-module corenetwork
  (export start-network-service)
  (import driver-util)
  ;; layer by layer: shared helpers, the checksum, then eth/arp/ip/icmp/udp, then
  ;; the service loop that ties them together.
  (include common checksum eth arp ip icmp udp service))
