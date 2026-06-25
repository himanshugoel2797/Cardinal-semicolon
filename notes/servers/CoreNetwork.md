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

## Lisp port status (the live stack under K5)

The live network stack is the Lisp `corenetwork` module (`lisp/servers/
corenetwork/`), a message-passing port of the C server. Beyond the C feature set
it now also carries the work that the C PRs #63 (DHCP) and #64 (outbound ARP)
prototyped, ported directly into Lisp so those PRs could close without merging:

- **DHCP client** (`corenetwork/dhcp.clp`): pure `dhcp-build`/`dhcp-parse` plus a
  per-interface client context (INIT→SELECTING→REQUESTING→BOUND→RENEWING, exp
  backoff, T1=lease/2 unicast renew). It is the **default**; `cardinal.ip=A.B.C.D`
  forces a static address and skips it. Started by `init` via the `dhcp-start`
  service message; the lease is written back with `set-address` and is readable
  with `get-address`. Verified live: a no-`cardinal.ip` virtio-net/slirp boot
  auto-acquires `10.0.2.15 gw 10.0.2.2 mask 255.255.255.0 lease 86400s`.
- **Outbound ARP** (`arp-resolve`, exported): cache-hit fast path, else broadcast
  who-has + poll, ~4 tries/~1s, then `#f`. Synchronous/task-context only (the
  caller supplies the next-hop IP — decoupled from routing).
- **ARP cache aging**: entries carry an expiry (`now + 120s`); an aged-out lookup
  reports a miss. Degrades to never-expire when no monotonic clock is calibrated.
- DHCP OFFER/ACK reception required relaxing IPv4 rx to also accept the limited
  broadcast `255.255.255.255` (the server broadcasts the reply while our address
  is still `0.0.0.0`); the C `ipv4_rx` did no dst filtering at all.

Still Lisp-side TODO (unchanged from the C deferrals below): a routing table that
uses the DHCP-learned gateway + `arp-resolve` for off-link TX, a DNS resolver,
RELEASE/DECLINE, and ARP-probe duplicate detection.

## Deliberately deferred (consequential design decisions — DO NOT guess)

These were left as notes rather than code because each constrains the system
design and should be decided deliberately, in line with the microkernel model:

1. **Userspace socket surface — *resolved by the architecture*.** This was
   originally framed as a ring-3 question (a `SysUser` syscall family? a `SysObj`
   object per socket?). In Cardinal; that framing is moot: **userspace is a
   capability-gated Lisp context**, not a ring-3 task, so the message-based socket
   API *is* the userspace surface. A sandboxed (`spawn-restricted`) context binds a
   UDP port or opens a TCP connection by messaging the network service and receives
   `udp-rx` / `tcp-rx` events back — exactly what `CoreNetDebug`'s echo handlers
   (empty-capability contexts) do. The capability grant + per-context heap are the
   isolation boundary. What genuinely remains is only the "raw queue" TODO for
   unhandled protocols.
2. **TX path architecture.** A proper transmit path wants a per-device tx queue,
   a tx worker task draining it to the driver, and outbound ARP resolution
   (hold the packet, emit an ARP request, retry/timeout on reply). *Outbound ARP
   resolution now exists in the Lisp stack (`arp-resolve`); the async tx-queue /
   packet-hold-during-resolution and waiter-coalescing remain.*
3. **Interface addressing / routing — *done (multi-homed host)*.** The stack is now
   multi-interface: each NIC `register-nic`s as a distinct interface with its own
   mac/tx and address config (`lisp/servers/corenetwork/route.clp`), and a
   longest-prefix-match **routing table** (`route-lookup`/`route-egress`) decides,
   per destination, the egress interface and next hop. `set-address` installs an
   interface's on-link route plus a default route via its gateway; replies egress on
   the interface that owns the addressed IP, active sends route by destination, and
   the limited broadcast (DHCP) goes out the primary interface. **No forwarding** —
   this is a multi-homed host, not a router (the firewall filters inbound on
   src-IP/port). **Multi-NIC bring-up + per-interface DHCP now work**: `init`
   enumerates every supported NIC (`pci-find-all`, so two virtio-nets or a
   virtio-net + an rtl all come up, each driver-init taking an explicit ECAM) and
   each registers as its own interface; `dhcp-start-all` then runs a DHCP client
   per interface. Each DISCOVER egresses on its OWN interface (`udp-send-if`, by
   MAC — not the routed/primary one), and UDP port 68 fans out to every client so
   each accepts only its own OFFER/ACK (filtered by xid/MAC). Verified live with
   two virtio-nets on distinct slirp subnets: `if0`→10.0.2.15, `if1`→10.0.3.15,
   each with its own gateway + routes. A DNS resolver exists (item below); a
   userspace config/query surface is moot (the message API is the userspace
   surface — see item 1).
4. **IPv4 options / fragmentation / reassembly.** Only `ihl == 5`, unfragmented
   datagrams are meaningfully handled.
5. **TCP.** *Implemented* (`lisp/servers/corenetwork/tcp.clp`): active + passive
   open, in-order data with cumulative ACKs, out-of-order reassembly (bounded
   reorder queue, trim-on-delivery so retransmits/overlaps neither double-deliver
   nor orphan), timeout retransmission (Go-Back-N, driven by a 100ms ticker
   context since the service cannot block), FIN teardown with TIME-WAIT, the
   pseudo-header checksum, and peer-window flow control. **Intentionally omitted**
   (LAN/loopback target): congestion control (slow-start/AIMD), SACK, window
   scaling, timestamps, delayed-ACK. Connection state lives in the single network
   service context (no locks) as mutable per-connection vectors keyed in hash
   tables. The socket surface is the in-OS *message* API (item 1), mirroring
   `udp-bind`: `tcp-listen`/`tcp-connect`/`tcp-send`/`tcp-close` to the service and
   `tcp-accept`/`tcp-connected`/`tcp-rx`/`tcp-closed` events to the owner, plus the
   synchronous `tcp-connect-blocking` open helper. Verified live (host `nc`/socket
   over slirp `hostfwd` against the `cardinal.netdbg` TCP echo on port 7):
   multi-segment transfer and clean four-way teardown. The **loss/reorder paths
   (retransmission + out-of-order reassembly) are validated** by a test-only fault
   injector: booting with `cardinal.tcploss` makes the stack drop 1 in 4 received
   segments (`tcp-test-drop?` in `tcp.clp`, off by default, one comparison per
   segment in production). Under that, an 8 KB multi-segment echo + teardown still
   completes byte-perfect — dropped data opens gaps that drive reassembly, and
   dropped ACKs make our retransmit timer fire. The socket API is already usable
   from sandboxed Lisp contexts (the userspace model — see item 1), so what remains
   is transport-level: routing over the learned gateway (item 3), a DNS resolver,
   and the async tx/packet-hold path (item 2).

   To run it: `cardinal.netdbg cardinal.tcploss` on the cmdline, then
   `NIC=virtio-net HOSTFWD="tcp::5555-:7" ./scripts/run-qemu.sh` (background) and a
   host socket to `localhost:5555` echoing a multi-KB payload; the boot log shows
   `[tcp-test] dropped inbound segment` lines and the echo returns intact.
6. **ARP aging.** *Timed expiry now exists in the Lisp stack (120s TTL, lazy drop
   on lookup).* A state machine for in-flight resolutions (coalescing concurrent
   waiters for the same IP, negative caching) remains deferred.
7. **Inbound firewall — *done*** (`lisp/servers/corenetwork/firewall.clp`). A small
   host packet filter: an ordered allow/deny rule list matched on (protocol,
   source IP/prefix, destination port) plus a default policy, evaluated in
   `handle-ip` before a received packet is dispatched to ICMP/UDP/TCP. First match
   wins; no match → default policy. Default is ALLOW with no rules, so it is inert
   until configured. It is a *host inbound* filter (no forwarding), so rules match
   the SOURCE address — which, since interfaces sit on distinct subnets, also
   stands in for the ingress interface without ingress tagging. Config messages:
   `(fw-policy allow|deny)`, `(fw-add action proto src-net src-len dport)` (any
   field may be `'any`), `(fw-clear)`, `(fw-query proto src-ip dport reply)`. The
   firewall lives in the service closure (mutated in place), not the threaded
   state. Validated: an in-OS self-test (`check_firewall`) over the rule/policy
   matrix, and live — booting `cardinal.fwtest` installs a `deny TCP dport 7` rule
   and the port-7 TCP echo becomes unreachable (SYN dropped) while DHCP/DNS/the
   listener still work.

## Testability

The pure logic (checksums, ARP-cache insert/lookup, frame/header building) is
written to be host-compilable and is covered by the host unit tests under
`tests/` (see that directory's README). The protocol I/O paths still need a
QEMU TAP/bridge or a packet capture to validate against a real peer.
