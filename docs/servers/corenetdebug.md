# corenetdebug

> An optional, command-line-gated network debug endpoint that exposes a UDP echo service, a UDP digest service, and a TCP echo service over the in-OS network stack.

| | |
|---|---|
| **Source** | `lisp/servers/corenetdebug.clp` |
| **Kind** | server |
| **Bound by** | `lisp/init.clp` — gated on `cardinal.netdbg` kernel command-line flag (always opt-in) |
| **Registers with** | [corenetwork](corenetwork.md) via `udp-bind` / `tcp-listen` |
| **Capabilities** | none — spawned with `spawn-restricted '()`; all network I/O flows through the `net` handle passed at init |

## Overview

`corenetdebug` is a lightweight debug transport. It is **inert by default**: the entire module is a no-op unless `start-netdebug` is called, which only happens when the kernel command line contains the flag `cardinal.netdbg`. This gate is intentional — the server opens network-reachable attack surface and must be explicitly enabled, exactly like the serial REPL.

Once started, `start-netdebug` registers three handler contexts with [corenetwork](corenetwork.md):

- **UDP echo on port 1337** — every received datagram is bounced back to the sender. Because the reply uses the captured source MAC from the `udp-rx` message, no ARP resolution is needed, making round-trip measurement possible even before ARP converges.
- **UDP digest on port 1338** — every received datagram is FNV-1a (32-bit) digested and the length and digest printed to the debug console. This is a single-datagram integrity check, so a host tool can confirm a blob arrived intact. The reliable, multi-packet path is the TCP echo on port 7 below.
- **TCP echo on port 7** — a connection-oriented echo server. It accepts inbound connections, bounces every received chunk back, and closes its side when the peer half-closes. This exercises the full TCP handshake, in-order delivery, retransmission, and FIN teardown.

Each endpoint runs in its own `spawn-restricted '()` context with no imported capabilities; all I/O is mediated by message-passing to and from the `net` corenetwork handle.

## Initialization

`init.clp` calls `start-netdebug` after the network stack is up and DHCP/static addressing has been initiated:

```scheme
(start-netdebug net)   ; → 'netdbg-up
```

- `net` — the [corenetwork](corenetwork.md) service handle obtained earlier in the boot sequence.
- Returns the symbol `'netdbg-up` on success.
- As a side effect, prints `[corenetdebug] enabled (udp echo 1337, digest 1338, tcp echo 7)` to the debug console.

`init.clp` gates the call:

```scheme
(if (cmdline-has? "cardinal.netdbg") (start-netdebug net))
```

To enable, add `cardinal.netdbg` to the GRUB/QEMU kernel command line. Example QEMU knob:

```
-append "cardinal.netdbg"
```

## Message protocol

`corenetdebug` does not expose a message-addressable handle to other Lisp contexts. It is a pure consumer of [corenetwork](corenetwork.md) callbacks. The descriptions below are the messages the internally-spawned handler contexts exchange with `net`.

### Inbound from corenetwork: `udp-rx`

Delivered to the echo and upload handler contexts by the UDP layer when a datagram arrives on a bound port.

- **Message shape:** `(udp-rx <src-ip> <src-mac> <src-port> <payload-bytes>)`
  - `src-ip` — source IPv4 address (list of 4 integers).
  - `src-mac` — source MAC address; used directly in the `udp-send` reply so no ARP lookup is required.
  - `src-port` — source UDP port (integer).
  - `payload-bytes` — datagram payload as a `bytes` buffer.

### Outbound to corenetwork: `udp-send` (echo handler)

Sent by the echo handler (port 1337) in response to every `udp-rx`:

```scheme
(send net (list 'udp-send src-ip src-mac ECHO-PORT payload src-port))
```

- `src-ip` / `src-mac` — captured from the inbound `udp-rx`.
- `ECHO-PORT` (1337) — the source port of the reply.
- `payload` — the original payload bytes, unmodified.
- `src-port` — destination port (the original sender's port).

### Upload / digest handler (port 1338)

No outbound `udp-send`; the handler prints the FNV-1a digest to the console:

```
[corenetdebug] upload len=<N> digest=<D>
```

where `<N>` is `(bytes-length payload)` and `<D>` is the 32-bit FNV-1a hash as a decimal integer.

### TCP messages (port 7)

The TCP echo server exchanges the following messages with `net`:

| Tag | Direction | Shape | Meaning |
|-----|-----------|-------|---------|
| `tcp-accept` | inbound | `(tcp-accept lport conn rip rport)` | New inbound connection established; `conn` is the connection handle. |
| `tcp-rx` | inbound | `(tcp-rx conn payload-bytes)` | In-order data received on `conn`. |
| `tcp-closed` | inbound | `(tcp-closed conn)` | Peer half-closed or connection aborted. |
| `tcp-send` | outbound | `(tcp-send conn payload-bytes)` | Echo the received data back to `conn`. |
| `tcp-close` | outbound | `(tcp-close conn)` | Begin active close (send FIN) when peer closes. |

Registration is done once at startup:

```scheme
(send net (list 'tcp-listen TCP-ECHO-PORT srv))   ; TCP-ECHO-PORT = 7
```

## Exported functions

### `(start-netdebug net)`

Spawns the UDP echo, UDP upload, and TCP echo handler contexts; binds them to their respective ports via [corenetwork](corenetwork.md); and returns `'netdbg-up`.

- `net` — corenetwork service handle.
- Side effect: three `spawn-restricted '()` contexts are alive for the lifetime of the OS. No explicit shutdown path exists.

No other public functions are exported. `fnv1a` and `start-tcp-echo` are module-private helpers.

## Notes / gotchas

**Opt-in only.** The server is a remote attack surface. Never enable `cardinal.netdbg` in a production image; it must be added to the kernel command line explicitly.

**No capabilities required.** Each handler context is spawned with `spawn-restricted '()` (empty capability list). All network access is through the `net` handle passed in at init. If `net` is invalid or the corenetwork context is dead, the handlers will block forever on `recv` or silently drop outbound `send`s.

**No ARP required for UDP echo.** The `udp-send` reply fills the destination MAC directly from the captured `src-mac` field of `udp-rx`. This means the echo path works even if no ARP entry for the host exists, which is useful for liveness testing immediately after link-up.

**UDP digest is single-datagram only.** Port 1338 digests each datagram independently: there is no reassembly, no acknowledgement, and no ordering guarantee across multiple datagrams. A host tool that sends a large blob in chunks will receive one digest line per datagram, not one for the whole blob. For a reliable multi-packet path use the TCP echo on port 7.

**TCP firewall test interaction.** `init.clp` supports a `cardinal.fwtest` command-line flag that installs a `deny` firewall rule for inbound TCP port 7. When both `cardinal.netdbg` and `cardinal.fwtest` are present, the TCP echo server is registered but unreachable; UDP echo (1337) and UDP digest (1338) remain functional.

**FNV-1a implementation.** The in-module `fnv1a` function uses the standard 32-bit FNV-1a parameters (offset basis `2166136261`, prime `16777619`) with explicit masking to 32 bits via `(bitwise-and … #xFFFFFFFF)`, so a host-side tool computing a standard FNV-1a digest will match.
