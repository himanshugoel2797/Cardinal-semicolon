# corenetwork

> The IPv4 network stack: Ethernet framing, ARP (cache + outbound resolution), IPv4 (receive/demux, fragmentation, reassembly), ICMP echo, UDP (bind/send), TCP (full state machine with retransmission), DHCP client, DNS resolver, inbound firewall, and an async TX path with ARP-hold queuing.

| | |
|---|---|
| **Source** | `lisp/servers/corenetwork.clp` (+ `lisp/servers/corenetwork/*.clp` — 16 component files) |
| **Kind** | server |
| **Bound by** | `lisp/init.clp` — always; started unconditionally as `(start-network-service)` before any NIC driver runs |
| **Registers with** | n/a — NIC drivers register *with* corenetwork via `register-nic` |
| **Capabilities** | none — corenetwork is a pure Lisp server; its spawned sub-contexts (`dhcp-client`, `dns-resolve`, tx-engine, TCP ticker) use `spawn-restricted '()` (no capability grants) |

## Overview

corenetwork is a single long-lived service context created by `start-network-service`. It owns the interface table (each NIC is an interface record with its MAC, TX context, and address config), the IPv4 routing table (longest-prefix match, multi-homed host — no forwarding), the ARP cache (120 s TTL), the UDP port-bind table, and the TCP connection tables (mutable hash tables indexed by 4-tuple and by connection id).

The module source is split across 16 component files in `lisp/servers/corenetwork/` that are spliced into the single module scope via `(include ...)`. None of the component files is importable on its own; the entire module exposes exactly four public symbols:

```scheme
start-network-service   ; → the service handle
arp-resolve             ; synchronous next-hop resolution (own context)
tcp-connect-blocking    ; synchronous active-open helper (own context)
dns-resolve             ; synchronous A-record lookup (own context)
```

Everything is message passing: the NIC sends `(rx frame len)`, the service demuxes it, and any reply is sent back to the NIC's TX context as a separate `(tx ...)` message. Because the RX handler never re-enters the TX path synchronously, there are no re-entrancy hazards and no locks.

Wire formats are big-endian; `put-be16!`/`get-be16` (from `driver-util`) do the byte assembly. The internet checksum (RFC 1071) is computed over big-endian 16-bit words in `checksum.clp`.

## Initialization

`init.clp` calls `start-network-service` before bringing up any NIC drivers, then passes the returned handle to each driver's init function:

```scheme
(start-network-service)   ; → net (the service handle)
```

`start-network-service` creates the TCP state, firewall, async TX hold-queue, fragment reassembly buffer, starts the TCP ticker context, and enters the `serve` loop. It returns the service handle.

After all NIC drivers have registered, `init.clp` sends address configuration:

```scheme
; static address (if cardinal.ip= cmdline arg present):
(send net '(set-address #f <ip> <netmask> <gw> <dns>))

; default: DHCP on every interface
(send net '(dhcp-start-all))
```

## Driver-facing API

NIC drivers receive the service handle as an argument to their init function and interact via two messages.

### `register-nic`

```scheme
(send net (list 'register-nic mac tx-ctx))
```

- **`mac`** — 6-element list of bytes, e.g. `'(0 1 2 3 4 5)`.
- **`tx-ctx`** — a context that receives `(tx frame len ack-target)` messages (raw frame bytes + length) and, once it has transmitted the frame, sends `(tx-done)` to `ack-target`. The service wraps this raw context in a per-interface TX engine (bounded FIFO + one-in-flight backpressure; see [TX engine](#tx-engine-txworkerclp)).

Each call creates one interface record. Multiple NICs are supported; each becomes a separate interface with its own address config and routing table entries.

### `rx`

```scheme
(send net (list 'rx frame len))
```

The driver sends this for every received ethernet frame. `frame` is a `bytes` object; `len` is its meaningful byte count. The service dispatches on the EtherType (0x0806 → ARP; 0x0800 → IPv4). Frames shorter than 14 bytes are silently dropped. The `frame` bytes must be a fresh copy (`copy-bytes`) since the service retains a reference for the duration of IP reassembly.

## Message Protocol

All messages are sent to the handle returned by `start-network-service`. The service loop is implemented with `serve` (a `recv`/loop helper); it serialises all messages, so there is no concurrency inside the service.

### Ethernet / ARP messages

#### `:arp-request`

```scheme
(send net (list 'arp-request ip))
```

- **Request:** `ip` is a 4-element byte list. The service emits an ARP who-has on the egress interface selected by the routing table for `ip` (limited broadcast `255.255.255.255` goes out the primary interface).
- **Reply:** none.

#### `:arp-lookup`

```scheme
(send net (list 'arp-lookup ip reply-ctx))
```

- **Request:** `ip` — 4-element byte list; `reply-ctx` — the context to reply to (typically `(self)`).
- **Reply:** sends the MAC (6-element list) to `reply-ctx`, or `#f` on cache miss or expired entry.
- **Note:** the ARP cache uses a 120-second TTL (`ARP-CACHE-TTL-NS`). An `uptime-ns` of 0 (uncalibrated clock) means entries never expire.

### Address / routing messages

#### `:set-address`

```scheme
(send net (list 'set-address mac ip netmask gateway dns))
```

- **`mac`** — 6-element list to identify which interface to configure, or `#f` to configure the first interface.
- **`ip`**, **`netmask`**, **`gateway`**, **`dns`** — 4-element byte lists. `gateway` of `IP-ANY` (`0.0.0.0`) means no default route.
- **Effect:** mutates the interface record in place; installs (or replaces) the interface's on-link route and, if a gateway is set, a default route (`0.0.0.0/0`).

#### `:get-address`

```scheme
(send net (list 'get-address reply-ctx))
```

- **Reply:** sends `(ip netmask gateway dns)` (four 4-element lists) of the primary configured interface to `reply-ctx`. Returns `(0.0.0.0 0.0.0.0 0.0.0.0 0.0.0.0)` if no interface has an address yet.
- **"Primary"** is the first interface with a non-zero IP; falls back to the first interface in any state.

#### `:route-query`

```scheme
(send net (list 'route-query dst reply-ctx))
```

- **`dst`** — 4-element destination IP.
- **Reply:** sends `(src-ip mac tx next-hop)` (the egress interface's IP, MAC, TX context, and next-hop IP) to `reply-ctx`, or `#f` if no route exists.

### UDP messages

#### `:udp-bind`

```scheme
(send net (list 'udp-bind port handler-ctx))
```

- **`port`** — integer destination port (0–65535).
- **`handler-ctx`** — a context that receives `(udp-rx src-ip src-mac src-port payload)` for every arriving datagram on `port`.
- **Effect:** appends `(port . handler-ctx)` to the bind table. Multiple handlers may be bound to the same port (all receive each datagram). This is how multiple DHCP clients (one per interface) coexist on port 68.

#### `:udp-unbind`

```scheme
(send net (list 'udp-unbind port))
```

Removes all bindings for `port`. Used by `dns-resolve` to release the ephemeral source port after a query completes.

#### `:udp-send`

```scheme
(send net (list 'udp-send dst-ip dst-mac sport dport payload))
```

- **`dst-ip`** — 4-element destination IP.
- **`dst-mac`** — 6-element next-hop MAC (the caller must supply this; use `arp-resolve` to obtain it).
- **`sport`**, **`dport`** — source and destination ports (integers).
- **`payload`** — `bytes` object.
- The egress is chosen by the routing table for `dst-ip`; the send is dropped if no route exists. Datagrams larger than the 1500-byte MTU are fragmented automatically.

#### `:udp-send-if`

```scheme
(send net (list 'udp-send-if mac dst-ip dst-mac sport dport payload))
```

Like `udp-send` but forces egress on the interface identified by `mac` (its source MAC). Used internally by the DHCP client so each per-interface client broadcasts on its own link, not on the primary interface.

#### `:udp-send-async`

```scheme
(send net (list 'udp-send-async dst-ip sport dport payload))
```

Fire-and-forget: the service routes `dst-ip`, resolves the next-hop MAC itself (holding the datagram and emitting ARP who-has on a cache miss), and flushes it once the reply arrives. Packets to the same unresolved next hop coalesce behind one ARP request. After 3 seconds without a reply the held frames are discarded. No caller-side `arp-resolve` is needed.

#### UDP handler delivery: `:udp-rx`

The context bound to a port receives:

```scheme
(udp-rx src-ip src-mac src-port payload)
```

- **`src-ip`** — 4-element source IP.
- **`src-mac`** — 6-element source MAC (learned from the ethernet header, not from ARP).
- **`src-port`** — integer source port.
- **`payload`** — `bytes` object (a fresh copy of the datagram body, stripped of the 8-byte UDP header).

### ICMP messages

#### `:ping`

```scheme
(send net (list 'ping dst-ip dst-mac id seq))
```

- **`dst-ip`** — 4-element destination IP.
- **`dst-mac`** — 6-element next-hop MAC.
- **`id`**, **`seq`** — 16-bit ICMP echo identifier and sequence number.
- Emits an ICMP echo request. Echo replies from the peer are handled internally (the type byte is flipped and the checksum recomputed); no message is delivered to a caller context.

### DHCP messages

#### `:dhcp-start`

```scheme
(send net '(dhcp-start))
```

Spawns a `dhcp-client` context for the primary interface (first interface in any state). The client binds UDP port 68, runs DISCOVER → SELECTING → REQUESTING → BOUND, and on a successful lease calls `set-address` to configure the interface. It then waits for T1 (lease/2) and renews by unicasting a REQUEST to the learned server MAC. A NAK or timeout falls back to a fresh DISCOVER cycle.

#### `:dhcp-start-all`

```scheme
(send net '(dhcp-start-all))
```

Spawns a `dhcp-client` context for every registered interface. This is the default in `init.clp`.

### TCP messages

#### `:tcp-listen`

```scheme
(send net (list 'tcp-listen port owner))
```

- **`port`** — integer local port to listen on.
- **`owner`** — context that receives `(tcp-accept lport conn rip rport)` when an inbound connection completes its handshake.
- Passive open. Multiple ports may be listened on simultaneously; only one listener per port is supported (the last `tcp-listen` for a port overwrites the previous one).

#### `:tcp-connect`

```scheme
(send net (list 'tcp-connect dst-ip dst-mac dport owner))
```

- **`dst-ip`** — 4-element destination IP.
- **`dst-mac`** — 6-element next-hop MAC (caller must resolve this, e.g. via `arp-resolve`; or use `tcp-connect-blocking` which handles this automatically).
- **`dport`** — integer destination port.
- **`owner`** — context that receives the handshake events.
- Active open. The local port is chosen from the ephemeral range (49152 + `uptime-ns` mod 16000). The SYN is sent immediately; the `owner` receives `(tcp-connected conn)` once the SYN-ACK arrives.

#### `:tcp-send`

```scheme
(send net (list 'tcp-send conn payload))
```

- **`conn`** — integer connection handle (returned in `tcp-connected` / `tcp-accept`).
- **`payload`** — `bytes` object. Appended to the connection's send queue; segments are emitted up to the peer's advertised window in MSS-sized (1024-byte) chunks. Valid in `established` and `close-wait` states.

#### `:tcp-close`

```scheme
(send net (list 'tcp-close conn))
```

Begins the active close: sets `C-FINQ`, transitions to `fin-wait-1` (from `established`) or `last-ack` (from `close-wait`), and emits the FIN once all queued data is acknowledged.

#### `:tx-stats`

```scheme
(send net (list 'tx-stats reply-ctx))
```

- **Reply:** forwards the request to the primary interface's TX engine; the engine replies `(queued dropped)` — current FIFO length and total dropped-frame count.

#### `:tcp-test-loss` (test only)

```scheme
(send net (list 'tcp-test-loss N))
```

Enables fault injection: drops 1 in N received segments (except SYNs), exercising out-of-order reassembly and retransmit paths. `N=0` disables injection. Not for production use.

### Firewall messages

The inbound firewall is applied to every received IPv4 packet before it reaches ICMP/UDP/TCP demux. The default policy is **allow** with no rules, so the firewall is inert until configured.

#### `:fw-policy`

```scheme
(send net (list 'fw-policy 'allow))   ; or 'deny
```

Sets the default action when no rule matches. `'allow` (the initial default) passes all unmatched packets; `'deny` drops them.

#### `:fw-add`

```scheme
(send net (list 'fw-add action proto src-net src-len dport))
```

Appends a rule. Rules are matched in insertion order; first match wins.

- **`action`** — `'allow` or `'deny`.
- **`proto`** — IP protocol number (e.g. `17` for UDP, `6` for TCP, `1` for ICMP) or `'any`.
- **`src-net`** — 4-element source IP prefix, or `'any`.
- **`src-len`** — prefix length in bits (0–32); used only when `src-net` is not `'any`.
- **`dport`** — integer destination port or `'any`. ICMP has no port; use `'any`.

#### `:fw-clear`

```scheme
(send net '(fw-clear))
```

Removes all rules. The default policy (set by `fw-policy`) continues to apply.

#### `:fw-query`

```scheme
(send net (list 'fw-query proto src-ip dport reply-ctx))
```

- **Reply:** sends `#t` (allow) or `#f` (deny) to `reply-ctx`.

## TCP owner events

A context registered as `owner` in `tcp-listen` or `tcp-connect` receives these messages:

### `tcp-accept`

```scheme
(tcp-accept lport conn rip rport)
```

An inbound connection has completed its three-way handshake. `lport` is the local port; `conn` is the integer connection handle; `rip` is the remote IP (4-element list); `rport` is the remote port.

### `tcp-connected`

```scheme
(tcp-connected conn)
```

The active-open SYN-ACK was received and acknowledged; the connection is now in the `established` state. `conn` is the handle to use with `tcp-send` and `tcp-close`.

### `tcp-rx`

```scheme
(tcp-rx conn payload)
```

In-order received data. `payload` is a `bytes` object. Out-of-order segments are buffered (up to 64 entries) and delivered in sequence once the gap is filled.

### `tcp-closed`

```scheme
(tcp-closed conn)
```

The connection is closed. This is sent when the peer's FIN arrives in order (in `established`, `fin-wait-1`, or `fin-wait-2` states) or when a RST is received. After this message the `conn` handle is invalid.

## Exported Functions

These four functions are exported from the `corenetwork` module and may be called from other contexts that `(import corenetwork)`.

### `(start-network-service)`

Creates and returns the service handle (a context). Should be called exactly once from `init.clp`. The returned handle is passed to NIC driver init functions.

### `(arp-resolve net ip)`

```scheme
(arp-resolve net ip)   ; → mac-list | #f
```

Synchronous next-hop MAC resolution. Sends `arp-lookup` to the service; on a cache miss, emits `arp-request` and polls again with 250 ms delays, up to 4 attempts (~1 second total). Returns the 6-element MAC list, or `#f` if the address is unreachable after all retries.

**Must run in its own context** — it blocks on `recv` for the service's reply and `sleep`s between tries. Never call it inside the service loop or while holding any lock.

### `(tcp-connect-blocking net dst-ip dst-mac dport)`

```scheme
(tcp-connect-blocking net dst-ip dst-mac dport)   ; → conn | #f
```

Synchronous active-open helper. Sends `tcp-connect` with `(self)` as owner, then blocks on `recv` until either `tcp-connected` (returns the handle) or `tcp-closed` (returns `#f`) arrives. Unrelated messages are discarded. After it returns, the caller receives `tcp-rx` and `tcp-closed` events on its own mailbox and drives the connection with `tcp-send` / `tcp-close`.

**Must run in its own context** — it blocks on `recv`.

### `(dns-resolve net name)`

```scheme
(dns-resolve net name)   ; → ip-list | #f
```

Synchronous A-record resolver. Queries `get-address` to find the DHCP-learned DNS server, resolves the next-hop MAC with `arp-resolve`, binds an ephemeral UDP port, sends a standard recursive query, and polls for a matching reply with 1 s/2 s/4 s exponential backoff (up to 3 tries). Unbinds the ephemeral port before returning. Returns a 4-element IP list, or `#f` on failure (no DNS server configured, interface not up, no route to server, or no A record in the reply).

**Must run in its own context** — it blocks via the poll loop.

## Internal Architecture

### TX engine (`txworker.clp`)

Each interface's TX context is not the raw NIC TX context but a per-interface TX engine spawned by `start-tx-engine`. The engine keeps at most one frame in flight (it forwards a frame to the NIC, then waits for the NIC's `tx-done` ack before sending the next) and holds up to 64 pending frames in a FIFO. Frames that arrive when the FIFO is full are dropped and counted. A `tx-tick` message (sent by the service every 100 ms) acts as a fallback ack for NICs that never send `tx-done`, pacing them at tick rate rather than stalling permanently.

The engine receives:
- `(tx frame len)` — enqueue a frame for transmission.
- `(tx-done)` — the NIC finished the previous frame; send the next.
- `(tx-tick)` — stale-frame fallback (see above).
- `(tx-stats reply-ctx)` — reply `(queued dropped)`.

### Async TX hold-queue (`txq.clp`)

`udp-send-async` uses a fire-and-forget path. If the next-hop MAC is not in the ARP cache, the service holds the frame (and any subsequent frames to the same next hop) and emits an ARP who-has. When an ARP reply arrives, the service calls `txq-flush!` to drain all held frames for the newly-resolved next hop. Entries are retried every 500 ms and dropped after 3 seconds.

### Fragment reassembly (`frag.clp`)

Inbound IPv4 fragments are buffered in a mutable reassembly table keyed by `(src-ip dst-ip proto id)`. Fragments are stored as a sorted `(offset . data)` list. When all fragments covering `[0, total)` are present, a complete unfragmented frame is synthesised and re-dispatched through `handle-ip`. Incomplete datagrams are evicted after 5 seconds. The table is bounded to 8 concurrent datagrams to limit memory under a fragment flood.

Outbound datagrams larger than the 1500-byte MTU (e.g. a large UDP send or a ping reply to an over-MTU request) are fragmented by `eth-tx-ip`: fragments share one IP identification counter (`frag-id-ctr`, a mutable cell), carry the MF flag, and use `/8`-aligned offsets.

### TCP state machine (`tcp.clp`)

TCP uses mutable hash tables and mutable connection vectors (kernel Scheme `make-vector`/`vector-set!`). All state lives inside the single service context — there are no additional contexts per connection and no locks. States tracked: `syn-sent`, `syn-rcvd`, `established`, `close-wait`, `fin-wait-1`, `fin-wait-2`, `closing`, `last-ack`, `time-wait`, `closed`.

Key parameters:
- MSS: 1024 bytes (advertised and enforced on send).
- Receive window: 16 384 bytes (fixed).
- Initial RTO: 500 ms; doubles on timeout, capped at 8 s (Go-Back-N retransmission).
- TIME-WAIT: 2 × 2 s = 4 s.
- Tick period: 100 ms (from the `start-tcp-ticker` context).
- Out-of-order reassembly queue: bounded to 64 entries per connection.

Absent features (deliberate, as noted in the source): congestion control, SACK, window scaling, TCP timestamps, and a higher-level socket API.

### DHCP client (`dhcp.clp`)

Each `dhcp-client` context binds UDP port 68 and implements INIT → SELECTING → REQUESTING → BOUND → RENEWING. It uses exponential backoff (1 s, 2 s, 4 s … 8 s cap, up to 5 tries per phase). Transaction IDs are derived from a per-module counter mixed with the interface's MAC bytes to avoid matching stale replies. Renewal unicasts to the server MAC learned from the OFFER. A NAK or renewal timeout triggers a full DISCOVER restart.

The idle wait before renewal uses a deadline-poll loop (`dhcp-idle`) rather than a bare `sleep`, because `sleep` and `recv` share the same blocked flag in the Lisp VM: any datagram arriving on port 68 would wake `sleep` early and spin the renew loop.

## Notes / Gotchas

### RX/TX is fully message-passing (no locks)

The service never re-enters a driver's TX path synchronously: the NIC sends `(rx ...)` as a message, the service processes it, and any outbound frame is sent back to the NIC's TX context as a separate `(tx ...)` message. **There are no re-entrancy hazards and no locks in corenetwork itself.**

If you are writing a NIC driver, observe:
- Never call into the service synchronously (no `send`+`recv` from inside a frame delivery path that already holds a lock).
- Pass a fresh `copy-bytes` of the receive buffer in every `(rx ...)` message; the service may retain the bytes across multiple message cycles for fragment reassembly.

### `sleep` wakes on `send`

The Lisp VM's `sleep` and `recv` share the blocked flag. A message arriving in a sleeping context's mailbox wakes it immediately. The DHCP client works around this with `dhcp-idle` (a deadline poll that drains stray mail); `dns-resolve` uses a similar `dns-poll` pattern. Do not use a bare `sleep` as a deadline wait in any context that may receive unsolicited mail.

### Ephemeral port collisions

`dns-resolve` picks an ephemeral source port as `30000 + (uptime-ns mod 20000)`. Multiple concurrent `dns-resolve` calls in different contexts may collide on the same port; all of them will receive each DNS reply and accept the first matching one. This is acceptable for the single-threaded boot path but is not safe for heavy concurrent DNS use.

### Multi-homed addressing

`set-address` with `mac #f` configures the first registered interface. For multi-homed hosts, pass the specific interface MAC to target the right interface. `get-address` returns the primary configured interface only; use `route-query` to inspect per-destination egress.

### TCP — no userspace socket API

TCP is fully wired at the service level, but there is no higher-level socket API yet — callers drive connections directly via the `tcp-*` messages and the owner-event protocol above. A userspace socket layer is noted as TODO in the source comments and in `notes/AUDIT.md`.

### Firewall ordering

Rules are evaluated in insertion order; the first match wins. `fw-clear` resets to the empty list, restoring the `allow`-all default (if `fw-policy allow` was set). Installed rules survive a `fw-clear` only if `fw-policy deny` was set before clearing; in that case the default becomes deny-all.

### Fragment flood protection

The reassembly table is bounded to 8 concurrent in-progress datagrams (`REASM-MAX`). A flood of distinct IP identification values fills the table and then evicts the oldest entry on each new arrival, preventing unbounded heap growth. Reassembled payloads are capped at 65 535 bytes (`REASM-MAX-LEN`).
