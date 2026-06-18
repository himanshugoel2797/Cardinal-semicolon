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
  interface's IP. Outbound: `arp_send_request` broadcasts a who-has, and
  `arp_resolve(iface, ip, mac_out)` resolves an unknown next-hop — cache hit
  returns immediately, else it broadcasts requests and polls the cache (which
  `arp_rx` fills) with short `task_sleep`s, ~4 tries / ~1s, then fails. It
  **sleeps, so it is task-context only** (never call from rx/interrupt); the
  caller picks the next-hop IP, so routing stays the caller's concern. Entries
  **age out** after `ARP_CACHE_TTL_NS` (120s); lookups drop an expired entry and
  report a miss (lazy aging, no sweep). Aging degrades to never-expire if no
  monotonic counter is calibrated.
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

## UDP + reliable delivery (RDT)

A general UDP layer and a reliable transport on top of it, exported for any
service to use (public headers in `servers/inc/CoreNetwork/`):

- **UDP** (`udp.c`, `CoreNetwork/udp.h`): a small spinlock-guarded port table.
  `udp_bind(port, handler, ctx)` / `udp_unbind(port)` register a handler; the
  rx demux (`udp_ipv4_rx`) validates the checksum (pseudo-header; `csum==0` means
  "absent"), looks up the bound port, and — crucially — copies the handler out
  **under the lock then releases it before calling**, so a handler may reply via
  `udp_send_to` without self-deadlocking. The handler is given the sender's
  ip/mac/port, so a reply needs no outbound ARP (we answer the captured MAC).
  `udp_send_to` builds the UDP header + pseudo-header checksum and sends via
  `ipv4_tx`. The pseudo-header sum is factored (`udp_ipv4_pseudo_sum`) so the rx
  verify and tx build paths cannot drift.
- **RDT** (`rdt.c`, `CoreNetwork/rdt.h`): a minimal **R**eliable **D**elivery
  **T**ransport for shipping a named blob to the device far faster than the serial
  bridge. It is **responder-driven and stop-and-wait**: the *host* owns all
  retransmission and the device only ACKs what it has, so there are **no
  device-side timers** and a dropped datagram costs one host timeout, never a
  device hang (the same design as the serial `recv_blob`). One datagram = one
  `rdt_hdr_t` + trailing (the blob name for START, a chunk for DATA). The device
  keeps a small transfer table (START allocates the buffer, DATA appends at a
  cumulative offset and ACKs `recd`, an out-of-order/duplicate DATA re-ACKs
  current progress) and a completed-transfer memo so a retransmitted final DATA
  re-ACKs FIN idempotently without re-running the sink. `rdt_listen(port, cb, ctx)`
  binds a UDP port and fires `cb` with the assembled blob. Hostile inputs are
  bounded: `RDT_MAX_BLOB` cap on the declared length, a fixed transfer-table size
  with round-robin eviction, and a per-datagram checksum.
- **Interface address override** (`net_dev.c`): `cardinal.ip=A.B.C.D` on the
  kernel command line overrides `NET_DEFAULT_IPV4` at registration time — a small
  stepping stone toward DHCP, and what lets a QEMU run pick the slirp address.
- **Consumer** (`servers/CoreNetDebug/`): a separate, optional server gated by the
  `cardinal.netdbg` cmdline flag. It binds UDP 1337 (echo, for liveness/latency)
  and `rdt_listen`s 1338 (reliable upload, logging the blob name/len/FNV-1a
  digest). Host side: `scripts/cardinal-net.py ping|upload`.

### Runtime validation (2026-06, QEMU slirp, virtio-net)

End-to-end with `cardinal.netdbg cardinal.ip=10.0.2.15` and
`-device virtio-net-pci -netdev user,hostfwd=udp::13370-:1337,hostfwd=udp::13380-:1338`:
`cardinal-net.py ping` got 5/5 echo replies, and `upload` of a 5000-byte blob
completed with 0 retransmits, the guest logging a digest identical to the host's.
(The `rtl8139` driver, by contrast, never delivers RX to `network_rx_packet`, so
it cannot drive this — a separate driver gap, noted in AUDIT.) The UDP demux,
checksum, `udp_send_to`, and full RDT state machine (reassembly, cumulative ACK,
duplicate/out-of-order re-ACK, oversize/bad-magic/bad-csum rejection) are also
covered by in-OS SysTest loopback tests that craft frames and capture tx.

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

1. **Userspace socket surface.** In-kernel services can now bind UDP ports
   (`udp_bind`) and run a reliable transport (`rdt_listen`), which unblocks
   kernel-side consumers like `CoreNetDebug`. What remains deferred is the
   *userspace* surface — how a userspace task binds a port and receives datagrams
   (a `SysUser` syscall family? a `SysObj` object per socket? a shared-memory
   ring?) — and the "raw queue" TODO for unhandled protocols. TCP also needs this.
2. **TX path architecture.** Outbound ARP resolution now exists as a synchronous,
   task-context `arp_resolve` (above). What remains deferred is the richer async
   path: a per-device tx queue, a tx worker task draining it to the driver, and
   packet-hold-during-resolution rather than a blocking resolve. The current
   inline-from-RX send and the broadcast-only `network_tx_packet` are still
   placeholders for that. The synchronous resolve was a deliberate choice to keep
   this decoupled from routing — revisit if a non-blocking sender is needed.
3. **Interface addressing / configuration.** The default IP is still build-time
   (`NET_DEFAULT_IPV4`), now overridable per-boot via the `cardinal.ip=` cmdline.
   Real addressing still needs per-interface config (registry or userspace) and
   **DHCP** (RDT-style UDP services make a DHCP client straightforward now).
   Subnet mask, gateway, and a routing table all live here.
4. **IPv4 options / fragmentation / reassembly.** Only `ihl == 5`, unfragmented
   datagrams are meaningfully handled.
5. **TCP.** Not started; needs the socket API (1) and the tx path (2) first.
6. **ARP aging.** Done: entries expire after a TTL (lazy, on lookup). Remaining
   refinements deferred: negative caching of failed resolutions, and a proper
   in-flight-resolution state machine (the synchronous `arp_resolve` re-broadcasts
   per call rather than coalescing concurrent waiters for the same IP).

## Path to TCP

Tracking what TCP needs, as the pieces land, so the remaining design work is
explicit:

- **Done** — UDP layer, an L4 RX-dispatch template (ICMP/UDP in `ip.c`), and the
  internet + pseudo-header checksum helpers (`checksum.h`).
- **Done** — outbound ARP resolution (`arp_resolve`) + cache aging, so a
  connection-initiating layer can obtain a next-hop MAC.
- **Done / easy** — timer-driven retransmission: `task_sleep(id, ns)` already
  backs DHCP's retransmit/renewal and ARP's resolve poll; a TCP RTO timer is the
  same primitive (no new infrastructure needed).
- **TODO — routing table (needs design).** The DHCP-learned gateway is stored on
  the interface, but nothing yet decides on-link-vs-via-gateway or selects a
  next-hop. `arp_resolve` deliberately takes the next-hop IP *from its caller*, so
  this can be designed independently of ARP. **This is the main open decision.**
- **TODO — async / non-blocking TX path.** `arp_resolve` blocks the calling task
  (fine for connection setup, not for a general fast path); the per-device tx
  queue + packet-hold-during-resolution from item 2 above is still placeholder.
- **TODO — the TCP layer itself**: the connection state machine + an in-kernel
  socket-ish API (callbacks, as RDT does), with the userspace socket surface
  (item 1) a further, separate decision.

## Testability

The pure logic (checksums, ARP-cache insert/lookup, frame/header building) is
written to be host-compilable and is covered by the host unit tests under
`tests/` (see that directory's README). The protocol I/O paths still need a
QEMU TAP/bridge or a packet capture to validate against a real peer.
