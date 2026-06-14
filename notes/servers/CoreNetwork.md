# CoreNetwork — status and design notes

## What works now (this pass)

The receive path is wired end-to-end for the bring-up case, and a minimal
transmit path exists to make the stack observably alive:

- **Ethernet** (`ethernet.c`): RX demux to ARP / IPv4 / IPv6 by ethertype with a
  unicast-or-broadcast destination filter. `ethernet_tx()` frames an L3 payload
  (src = interface MAC, given dst MAC + ethertype), zero-pads short frames up to
  the 60-byte minimum, and hands the frame to the driver's `tx` callback.
- **ARP** (`arp.c`): a fixed 32-entry IP→MAC cache (`arp_cache_update` /
  `arp_cache_lookup`, spinlock-guarded, round-robin eviction). `arp_rx` validates
  ethernet/IPv4 ARP, learns the sender, and replies to requests addressed to the
  interface's IP.
- **IPv4** (`ip.c`): header-checksum verify (over `ihl*4`), version/ihl sanity,
  opportunistic ARP learning from any valid datagram, protocol demux. `ipv4_tx()`
  builds a header (ttl 64, df=0, computed checksum) and sends via `ethernet_tx`.
- **ICMP** (`icmp.c`): echo-request → echo-reply (the classic "is it alive?"
  path). Message length is taken from the IPv4 `total_len`, **not** the `len`
  argument from the driver (see the length caveat below).
- **Checksums** (`checksum.h`): one shared RFC-1071 implementation
  (`net_checksum16`, plus a partial-accumulate API for future pseudo-headers),
  computed in native byte order — see the header for why that validates on a peer
  of either endianness.

### How to exercise it
Default interface IP is `10.0.2.15` (`NET_DEFAULT_IPV4`), the address QEMU's
user-mode (slirp) network assigns the guest. ARP for the guest and (slirp
permitting) ICMP echo should now be answered. Note QEMU slirp's ICMP support is
limited and host→guest reachability is restricted, so a packet-trace or a
TAP/bridge setup is the reliable way to verify, not a host `ping`.

## Known caveats in the current code

- **Driver RX length is unreliable.** `drivers/virtio/net` passes a hard-coded
  `1514` to `network_rx_packet` rather than the real frame length. The RX path is
  therefore written to derive payload sizes from header fields (IPv4 `total_len`,
  fixed ARP size) and must not trust the `len` argument. Fixing the driver to
  report the actual used-buffer length is a prerequisite for anything that needs
  exact bounds (e.g. trailing-data handling, raw delivery).
- **TX from RX context.** Replies (ARP, ICMP) are sent synchronously from the
  driver's RX callback, which runs under `cli()`. This is fine for the current
  reply-only traffic but is not a general design (see "tx path" below).
- **UDP** RX checksum code predates this pass and is left as-is; it is not yet
  wired to any delivery mechanism.

## Deliberately deferred (consequential design decisions — DO NOT guess)

These were left as notes rather than code because each constrains the system
design and should be decided deliberately, in line with the microkernel model:

1. **Socket / port API + userspace surface.** How services and userspace bind
   ports and receive datagrams (a `SysUser` syscall family? a `SysObj` object
   per socket? a shared-memory ring?). This is the biggest decision and gates
   UDP/TCP delivery, the "raw queue" TODOs, and any real application.
2. **TX path architecture.** A proper transmit path wants a per-device tx queue,
   a tx worker task draining it to the driver, and outbound ARP resolution
   (hold the packet, emit an ARP request, retry/timeout on reply). The current
   inline-from-RX send and the broadcast-only `network_tx_packet` are
   placeholders for this.
3. **Interface addressing / configuration.** Static IP is hard-coded
   (`NET_DEFAULT_IPV4`). Real addressing needs per-interface config (registry or
   userspace) and **DHCP**. Subnet mask, gateway, and a routing table all live
   here.
4. **IPv4 options / fragmentation / reassembly.** Only `ihl == 5`, unfragmented
   datagrams are meaningfully handled.
5. **TCP.** Not started; needs the socket API (1) and the tx path (2) first.
6. **ARP aging.** The cache has no timed expiry or a state machine for in-flight
   resolutions; entries only get overwritten under pressure.

## Testability

The pure logic (checksums, ARP-cache insert/lookup, frame/header building) is
written to be host-compilable and is covered by the host unit tests under
`tests/` (see that directory's README). The protocol I/O paths still need a
QEMU TAP/bridge or a packet capture to validate against a real peer.
