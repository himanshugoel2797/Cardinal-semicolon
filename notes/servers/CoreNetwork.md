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
  length validation, protocol demux. `ipv4_tx()` builds a header (ttl 64, df=0,
  computed checksum) and sends via `ethernet_tx`.
- **ICMP** (`icmp.c`): echo-request → echo-reply (the classic "is it alive?"
  path). Message length is taken from the IPv4 `total_len`, **not** the `len`
  argument from the driver (see the length caveat below).
- **Checksums** (`checksum.h`): one shared RFC-1071 implementation
  (`net_checksum16`, plus a partial-accumulate API for future pseudo-headers),
  computed in native byte order — see the header for why that validates on a peer
  of either endianness.

### Runtime validation (2026-06, QEMU slirp)

Validated end-to-end and bidirectionally against QEMU's slirp peer via a
temporary boot self-test (`network_debug_selftest`, gated behind a `CALL:` in
`servicescript.txt`) plus a `filter-dump` packet capture. The capture showed the
full exchange with all checksums clean (tcpdump `-vv` flagged none):

```
52:54:00:12:34:56 > ff:ff:ff:ff:ff:ff  ARP Request who-has 10.0.2.2 tell 10.0.2.15
52:55:0a:00:02:02 > 52:54:00:12:34:56  ARP Reply 10.0.2.2 is-at 52:55:0a:00:02:02   (learned)
10.0.2.15 > 10.0.2.2  ICMP echo request id 4660 seq 1                               (our tx)
10.0.2.2 > 10.0.2.15  ICMP echo reply   id 4660 seq 1                               (accepted by peer)
```

This exercises ethernet framing, ARP request build + reply parse + cache update,
IPv4 header build + checksum, ICMP echo build + checksum, and inbound IPv4/ICMP
parsing. The inbound-echo-*reply* path (replying to a ping addressed to us) was
not directly driven (slirp does not easily route host→guest ICMP), but every
primitive it uses is proven by the above. Two findings came out of this run (both
in `notes/AUDIT.md`): a now-fixed **virtio-net 10-vs-12-byte header bug** that was
shifting every tx frame left by 2 bytes, and a pre-existing **KVM boot hang**
(boots only under `-accel tcg`; reproduces on master).

### How to exercise it
Default interface IP is `10.0.2.15` (`NET_DEFAULT_IPV4`), the address QEMU's
user-mode (slirp) network assigns the guest. ARP for the guest and (slirp
permitting) ICMP echo should now be answered. Note QEMU slirp's ICMP support is
limited and host→guest reachability is restricted, so a packet-trace or a
TAP/bridge setup is the reliable way to verify, not a host `ping`.

## RX length handling (hardened)

The receive path treats all header length fields as untrusted. The virtio-net
driver now reports the **real** frame length (from the used-ring `len`, clamped
to the rx buffer's data region) instead of a hard-coded `1514`, and every layer
bounds the lengths it derives against the bytes actually received before reading
them:

- `ethernet_rx` drops runts shorter than an ethernet header.
- `ipv4_rx` validates `ihl*4` and `total_len` against `len` (and `version == 4`,
  `ihl >= 5`) before the header checksum, and passes the validated header/payload
  lengths down.
- `icmp_ipv4_rx` locates the ICMP message at `ihl*4` (options-correct) and reads
  exactly `total_len - ihl*4` bytes, which the caller has confirmed fit the frame.

This closes the remotely-triggerable over-read where a crafted `total_len`/`ihl`
would have driven the checksum and reply `memcpy` past the 2 KiB rx buffer. Note
that opportunistic ARP learning from arbitrary IP datagrams was intentionally
**removed** (it was unauthenticated cache-poisoning surface with no consumer —
the ICMP reply path uses the received frame's own L2 source, and ARP entries are
learned only from actual ARP traffic).

## Known caveats in the current code

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
