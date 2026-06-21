# Core servers — public C API

The `Core*` OS services expose their public interfaces as C headers under
`servers/inc/`. Drivers attach to a service by filling in a descriptor and
calling that service's `*_register` function; events flow back the other way
through service-exported helpers. Each service keeps a small lock-protected
registry of descriptors.

Several of these servers are now *also* implemented in Lisp (`lisp/servers/`),
but the **C registration ABI documented here** — the `servers/inc/*` headers and
their C definitions under `servers/*/src/` — is what C drivers link against, and
is what these entries describe. Where the live service logic has moved to Lisp,
the body of the relevant entry says so.

---

## `display_desc_t`

- **kind:** struct
- **lang:** c
- **source:** `servers/inc/CoreDisplay/display.h`
- **hash:** e9d569c0a9da6e14

A display device a driver registers with CoreDisplay: its name, connection type, feature flags, opaque `state`, and the `display_handlers_t` callback table.

**Fields**

- `display_name[256]` — human-readable name.
- `connection` — a `display_connection_t` (`unkn`/`hdmi`/`DP`).
- `handlers` — the `display_handlers_t` callback table (below).
- `features` — bitwise OR of `display_features_t` flags.
- `state` — opaque driver pointer passed back as the first argument of every handler.

The driver owns the storage for this struct; CoreDisplay keeps the pointer in its
list, so it must remain valid until `display_unregister`.

---

## `display_handlers_t`

- **kind:** struct
- **lang:** c
- **source:** `servers/inc/CoreDisplay/display.h`
- **hash:** 4df97ede08e149d8

The callback table a display driver provides: set resolution/brightness/power, query framebuffer address, connection status, and display info, plus a flush.

Every callback takes the `display_desc_t.state` pointer as its first argument and
returns `int` (`0` on success). `get_framebuffer` writes the framebuffer base to
`*addr`; `get_status` writes a `display_status_t`; `get_displayinfo` fills a
`display_res_info_t` array and writes the entry count to `*entcnt`. `flush` is
required for drivers whose `features` include `display_features_requireflip`.

---

## `display_res_info_t`

- **kind:** struct
- **lang:** c
- **source:** `servers/inc/CoreDisplay/display.h`
- **hash:** bb5ba009d67405b9

One display mode: width, height, stride (all in pixels) and refresh rate.

---

## `display_info_t`

- **kind:** struct
- **lang:** c
- **source:** `servers/inc/CoreDisplay/display.h`
- **hash:** b9da25f436a401bd

Aggregate physical/current display description: physical size (`h_sz`/`w_sz`), bit depth, the current/total resolution count, and a pointer to a `display_res_info_t` array.

---

## `display_connection_t`

- **kind:** enum
- **lang:** c
- **source:** `servers/inc/CoreDisplay/display.h`
- **hash:** 6b498fe725318427

Display physical connection type: `display_connection_unkn`, `display_connection_hdmi`, `display_connection_DP`.

---

## `display_status_t`

- **kind:** enum
- **lang:** c
- **source:** `servers/inc/CoreDisplay/display.h`
- **hash:** c7f2cbb5e2cc5d02

Hot-plug status reported by a display's `get_status` handler: `display_status_disconnected` (0) or `display_status_connected` (1).

---

## `display_features_t`

- **kind:** enum
- **lang:** c
- **source:** `servers/inc/CoreDisplay/display.h`
- **hash:** afe4e01da53926fe

Bit flags for `display_desc_t.features`: `display_features_autoresize`, `display_features_requireflip` (driver needs an explicit `flush`), `display_features_hardware3d`.

---

## `display_register`

- **kind:** function
- **lang:** c
- **source:** `servers/CoreDisplay/src/main.c`
- **hash:** 5245175aae9f3ba3

Register a display device descriptor with CoreDisplay.

**Parameters**

- `desc` — pointer to a caller-owned `display_desc_t`; stored by pointer (must stay valid).

**Returns** `0` on success, `-1` if `desc` is `NULL`, `-2` if the internal list
append fails.

The descriptor pointer is appended to CoreDisplay's display list under a
spinlock. If no display is registered by post-init, CoreDisplay falls back to
loading `lfb.celf`. Note CoreDisplay now also has a Lisp implementation
(`lisp/servers/coredisplay.clp`); this C entry point is the ABI C drivers (e.g.
`lfb`, `virtio-gpu`) link against.

---

## `display_unregister`

- **kind:** function
- **lang:** c
- **source:** `servers/CoreDisplay/src/main.c`
- **hash:** 10498dd3b57baf44

Remove a previously registered display descriptor.

**Parameters**

- `desc` — the same pointer passed to `display_register`.

**Returns** `0` if the descriptor was found and removed, `1` if it was not in the
list, `-1` if `desc` is `NULL`.

---

## `edid_t`

- **kind:** struct
- **lang:** c
- **source:** `servers/inc/CoreDisplay/edid.h`
- **hash:** 491bbc67c19b4a40

Parsed EDID block: port type, bit depth, gamma, the established-timings bitmap, up to 8 standard timings, the 13-byte display name, and up to 4 detailed modes.

`gamma` is encoded as the raw byte; the real value is `gamma / 100 + 1`.
`established_modes` is the EDID established-timings bitmap (bytes 35–37). See
`edid_standard_timings_t` and `edid_detailed_mode_t` for the array element types.

---

## `edid_detailed_mode_t`

- **kind:** struct
- **lang:** c
- **source:** `servers/inc/CoreDisplay/edid.h`
- **hash:** 0f2e94e2dddecc23

One EDID detailed-timing descriptor: active/blank/sync-porch/sync-pulse counts for both axes, polarities, borders, pixel clock, and physical size in mm.

---

## `edid_standard_timings_t`

- **kind:** struct
- **lang:** c
- **source:** `servers/inc/CoreDisplay/edid.h`
- **hash:** 58cbde3929fd1369

One EDID standard-timing entry: horizontal/vertical resolution, vertical refresh, and the aspect-ratio numerator/denominator.

---

## `edid_port_type_t`

- **kind:** enum
- **lang:** c
- **source:** `servers/inc/CoreDisplay/edid.h`
- **hash:** 15d71b3467a59b1f

EDID digital-input port type: undefined, HDMI-a, HDMI-b, MDDI, or DisplayPort.

---

## `coredisplay_parse_edid`

- **kind:** function
- **lang:** c
- **source:** `servers/CoreDisplay/src/edid.c`
- **hash:** 0ee97025869c4e4e

Parse a raw 128-byte EDID block into an `edid_t`.

**Parameters**

- `raw` — pointer to the raw EDID bytes (at least one 128-byte block).
- `result` — caller-allocated `edid_t` filled on success.

**Returns** `true` if the EDID header magic (`00 FF FF FF FF FF FF 00`) is valid
and the block parsed, `false` otherwise. Only digital inputs are handled;
detailed modes, standard timings, the established-timings bitmap, and the display
name are extracted.

---

## `input_device_desc_t`

- **kind:** struct
- **lang:** c
- **source:** `servers/inc/CoreInput/input.h`
- **hash:** 7a1714a0a4deeafb

An input device a driver registers with CoreInput: name, feature flags, the `input_device_handlers_t` poll callbacks, device type, and opaque `state`.

`type` is an `input_device_type_t` (mouse/keyboard/controller/…). `features` may
include `input_device_features_absolutepos` for absolute-position pointers. The
descriptor is stored by pointer and must stay valid until unregister.

---

## `input_device_handlers_t`

- **kind:** struct
- **lang:** c
- **source:** `servers/inc/CoreInput/input.h`
- **hash:** 74c4717e59787aa6

A polled input device's callbacks: `has_pending(state)` and `read(state, events)`.

CoreInput's updater task polls every registered device: while `has_pending`
returns true it calls `read` to drain one `input_device_event_t`. Both callbacks
run under CoreInput's device-list lock, so they must not block.

---

## `input_device_event_t`

- **kind:** struct
- **lang:** c
- **source:** `servers/inc/CoreInput/input.h`
- **hash:** 681cb0e86a46f49a

A single input event: timestamp, an `is_btn_event` discriminator, the control `index`, and a union of `bool state` (buttons) or `float position` (axes).

Event timestamp ordering across devices is not yet implemented: events are
enqueued in device-poll order.

---

## `input_device_type_t`

- **kind:** enum
- **lang:** c
- **source:** `servers/inc/CoreInput/input.h`
- **hash:** 9f4973d1d5fbb984

Input device class: unknown, mouse, keyboard, controller, joystick, throttle, touchpad, touchscreen.

---

## `input_device_features_t`

- **kind:** enum
- **lang:** c
- **source:** `servers/inc/CoreInput/input.h`
- **hash:** e9c11fa882a14820

Input device feature flags: `input_device_features_none`, `input_device_features_absolutepos`.

---

## `kbd_keys_t`

- **kind:** enum
- **lang:** c
- **source:** `servers/inc/CoreInput/input.h`
- **hash:** 454366c624bd6865

The canonical platform-independent keycode enumeration (letters, digits, function/keypad/navigation/modifier keys) keyboard drivers map scancodes to.

`kbd_keys_unkn` (0) is the unmapped/unknown key. These values populate the
`index` field of a keyboard `input_device_event_t`.

---

## `input_device_register`

- **kind:** function
- **lang:** c
- **source:** `servers/CoreInput/src/main.c`
- **hash:** 06bf76c068944946

Register an input device with CoreInput so its events are polled into the global event queue.

**Parameters**

- `desc` — caller-owned `input_device_desc_t`; stored by pointer.

**Returns** `0` on success, `-1` if `desc` is `NULL` or either of the required
`has_pending`/`read` handlers is `NULL`, `-2` if the list append fails.

CoreInput also has a Lisp implementation (`lisp/servers/coreinput.clp`); this is
the C registration ABI.

---

## `input_device_unregister`

- **kind:** function
- **lang:** c
- **source:** `servers/CoreInput/src/main.c`
- **hash:** e8495d34996a0656

Remove a previously registered input device.

**Parameters**

- `desc` — the same pointer passed to `input_device_register`.

**Returns** `0` if found and removed, `1` if not in the list, `-1` if `desc` is
`NULL`.

---

## `network_device_desc_t`

- **kind:** struct
- **lang:** c
- **source:** `servers/inc/CoreNetwork/driver.h`
- **hash:** 7e5d38404dd1188b

A NIC a driver registers with CoreNetwork: name, opaque `state`, feature flags, device type (ethernet/wifi), 6-byte MAC, a per-type handler union (tx + link/scan), and an embedded per-device `lock`.

The `handlers` and `spec_features` unions are selected by `type`. For an ethernet
device the `ether.tx(state, packet, len, flags)` callback transmits a frame and
`ether.link_status(state)` reports link state. `flags` is an OR of
`network_device_tx_flags_t` requesting hardware checksum offload. The embedded
`lock` is taken by CoreNetwork during registration.

---

## `network_ethernet_handlers_t`

- **kind:** struct
- **lang:** c
- **source:** `servers/inc/CoreNetwork/driver.h`
- **hash:** a11947db4ba8cf10

The ethernet NIC callback table: `tx(state, packet, len, flags)` and `link_status(state)`.

---

## `network_wifi_handlers_t`

- **kind:** struct
- **lang:** c
- **source:** `servers/inc/CoreNetwork/driver.h`
- **hash:** 8807bcff17ecc24b

The Wi-Fi NIC callback table: `tx(packet, len, flags)` plus stub scan handlers (`start_scan`/`finish_scan`/`abort_scan`).

Scan support is a TODO; the structure is defined but not yet wired into a scan
flow.

---

## `network_device_type_t`

- **kind:** enum
- **lang:** c
- **source:** `servers/inc/CoreNetwork/driver.h`
- **hash:** 07f03fe35632a774

NIC class selecting the handler/features union: `network_device_type_ethernet` (0), `network_device_type_wifi` (1), `network_device_type_count`.

---

## `network_device_features_t`

- **kind:** enum
- **lang:** c
- **source:** `servers/inc/CoreNetwork/driver.h`
- **hash:** 41fea270426cc721

Device-level NIC feature flags: `network_device_features_checksum_offload`.

---

## `network_device_tx_flags_t`

- **kind:** enum
- **lang:** c
- **source:** `servers/inc/CoreNetwork/driver.h`
- **hash:** b26533d35f00f7a2

Per-transmit offload request flags passed to a NIC's `tx`: IPv4, TCPv4, and UDPv4 checksum offload.

---

## `network_register`

- **kind:** function
- **lang:** c
- **source:** `servers/CoreNetwork/src/net_dev.c`
- **hash:** 0306d1fa3e188ff6

Register a NIC with CoreNetwork, creating an interface and assigning it an IPv4 address.

**Parameters**

- `desc` — caller-owned `network_device_desc_t` describing the NIC.
- `network_handle` — out; receives the opaque interface handle to pass to
  `network_rx_packet` and that handlers receive as `iface`.

**Returns** `0` on success, `-1` on allocation failure.

CoreNetwork copies the descriptor into a freshly allocated `interface_def_t`,
indexes it by type, and assigns an IPv4 address from the `cardinal.ip=A.B.C.D`
kernel command line (falling back to the build-time default — useful for the QEMU
slirp `10.0.2.15`). The descriptor's embedded `lock` is held while reading its
fields. CoreNetwork is also implemented in Lisp (`lisp/servers/corenetwork/`);
this is the C ABI NIC drivers register through.

---

## `network_rx_packet`

- **kind:** function
- **lang:** c
- **source:** `servers/CoreNetwork/src/net_dev.c`
- **hash:** c23127e417c48922

Hand a received frame to CoreNetwork's stack for synchronous processing.

**Parameters**

- `interface_handle` — the handle `network_register` wrote.
- `packet` / `len` — the received L2 frame and its length.

**Returns** the per-type RX result (`ethernet_rx`/`wifi_rx`), or `-1` for an
unknown device type.

This is callable from any thread but **runs the network stack synchronously**:
for a frame that needs a reply (ARP/ICMP, or a UDP service that answers via
`udp_send_to`) it re-enters the *same* driver's TX path. **Never hold a driver
lock across `network_rx_packet`** — doing so self-deadlocks the moment a
reply-triggering frame arrives.

---

## `udp_handler_t`

- **kind:** typedef
- **lang:** c
- **source:** `servers/inc/CoreNetwork/udp.h`
- **hash:** bd2fdcbd58d34c1f

Callback type for a bound UDP port: receives `(ctx, iface, src_ip, src_mac, src_port, dst_port, payload, len)` for each datagram.

`src_ip` is network byte order; `src_mac` is the sender's 6-byte L2 address, so
replying to that `iface`/`src_ip`/`src_mac` pair via `udp_send_to` needs no ARP.
The handler runs in the driver RX context with the binding lock already released,
so calling `udp_send_to` from within it is safe; it must not block.

---

## `udp_bind`

- **kind:** function
- **lang:** c
- **source:** `servers/CoreNetwork/src/udp.c`
- **hash:** 2db3b4c2a2bdff1a

Bind a handler to a destination UDP port (host order).

**Parameters**

- `port` — host-order destination port (must be non-zero).
- `handler` — `udp_handler_t` to invoke per datagram (must be non-NULL).
- `ctx` — opaque pointer passed back to the handler.

**Returns** `0` on success; `-1` on bad args, a port already bound, or no free
binding slot (table holds `UDP_MAX_BINDINGS` = 16 entries).

---

## `udp_unbind`

- **kind:** function
- **lang:** c
- **source:** `servers/CoreNetwork/src/udp.c`
- **hash:** fb9158690f6174e3

Remove the handler binding for a UDP port.

**Parameters**

- `port` — host-order port previously bound.

**Returns** `0` if a binding was removed, `-1` otherwise.

---

## `udp_send_to`

- **kind:** function
- **lang:** c
- **source:** `servers/CoreNetwork/src/udp.c`
- **hash:** fba0eb7ac3f0d049

Build and transmit a UDP/IPv4 datagram to a specific L2 address over an interface.

**Parameters**

- `iface` — the interface handle (from `network_register`, or the `iface` a
  handler received).
- `dst_ip` — destination IPv4, network byte order.
- `dst_mac` — destination 6-byte L2 address (typically the `src_mac` captured
  from a received datagram, so no ARP is needed).
- `src_port` / `dst_port` — host order.
- `payload` / `len` — the UDP body.

**Returns** `0` on success, negative on bad args, overflow of the 16-bit UDP
length, allocation failure, or a TX failure. The IPv4+UDP headers and the UDP
checksum are built here; a computed checksum of zero is sent as `0xFFFF` per
RFC 768. Safe to call from a `udp_handler_t` (the bind table lock is released
before handlers run).

---

## `rdt_hdr_t`

- **kind:** struct
- **lang:** c
- **source:** `servers/inc/CoreNetwork/rdt.h`
- **hash:** 512301628f59597a

The wire header for the Reliable Delivery Transport (RDT) over UDP: `CRDT` magic, message type/flags, transfer id, total/offset lengths, name/chunk lengths, and a trailing internet checksum.

All multi-byte fields are network byte order; the `csum` field (which must be
last) is a standard internet checksum over the header (excluding its own two
bytes) plus the trailing payload bytes. One datagram carries one header followed
by START name bytes, DATA chunk bytes, or nothing (ACK). See the header's block
comment for the full stop-and-wait, host-driven exchange.

---

## `RDT_TYPE_START`

- **kind:** constant
- **lang:** c
- **source:** `servers/inc/CoreNetwork/rdt.h`
- **hash:** 513656def25beb3e

RDT message-type values for `rdt_hdr_t.type`: `RDT_TYPE_START` (1), `RDT_TYPE_DATA` (2), `RDT_TYPE_ACK` (3, device→host only).

---

## `RDT_FLAG_FIN`

- **kind:** macro
- **lang:** c
- **source:** `servers/inc/CoreNetwork/rdt.h`
- **hash:** 3bc1ae3cd6de8d6e

Flag set on the ACK that completes a transfer; a retransmitted final DATA re-ACKs FIN without re-running the completion callback.

---

## `RDT_MAX_BLOB`

- **kind:** macro
- **lang:** c
- **source:** `servers/inc/CoreNetwork/rdt.h`
- **hash:** acb21e2616c59887

The largest blob a single RDT transfer may declare (16 MiB) — a protocol cap bounding device memory against a hostile START.

`RDT_MAX_NAME` (64) caps the blob name length. The `RDT_MAGIC0..3` macros are the
`'C' 'R' 'D' 'T'` header magic bytes.

---

## `rdt_complete_t`

- **kind:** typedef
- **lang:** c
- **source:** `servers/inc/CoreNetwork/rdt.h`
- **hash:** 69cd49f8f724170f

Callback type invoked once when an RDT blob has fully arrived: `(ctx, iface, src_ip, src_mac, src_port, name, data, len)`.

`name` is a NUL-terminated copy and `data`/`len` are the assembled blob; both are
owned by the transport and valid only for the duration of the call — do not
retain them. The callback runs in the receive path, so it must not block.

---

## `rdt_listen`

- **kind:** function
- **lang:** c
- **source:** `servers/CoreNetwork/src/rdt.c`
- **hash:** 427924298eedf27a

Listen for RDT transfers on a UDP port, invoking a callback on each completed blob.

**Parameters**

- `port` — host-order UDP port to listen on (non-zero).
- `on_complete` — `rdt_complete_t` run once per fully received blob (non-NULL).
- `ctx` — opaque pointer passed back to the callback.

**Returns** `0` on success; `-1` on bad args, no free listener slot
(`RDT_MAX_LISTENERS`), or if the underlying `udp_bind` fails (e.g. port already
taken). Intended to be called at module init, not concurrently; it claims a
listener slot then binds the RDT UDP handler to `port`.

---

## `storage_blockdev_t`

- **kind:** struct
- **lang:** c
- **source:** `servers/inc/CoreStorage/storage.h`
- **hash:** bc04b6d29586b03e

A block device a driver registers with CoreStorage: name, opaque `state`, block size/count, `read`/`write` callbacks, and the CoreStorage-internal `claimed` flag.

`read`/`write` transfer `count` blocks starting at `lba`, returning `0` on
success and `<0` on error; they may be called concurrently and the driver must do
its own serialization. Drivers must **not** set `claimed` — CoreStorage zeros it
on registration and sets it once a filesystem provider mounts the device, so it
is never offered twice. The descriptor is **copied** into a CoreStorage-owned
allocation on registration.

---

## `storage_fsprovider_t`

- **kind:** struct
- **lang:** c
- **source:** `servers/inc/CoreStorage/storage.h`
- **hash:** 7b63e51621984bed

A filesystem provider (e.g. cardfs, tarfs) that plugs into CoreStorage: a `name` and a `probe(blockdev_handle)` callback.

`probe` is a read-only "is this volume mine?" check that must NOT format or mutate
an unrecognized device; it returns `0` to claim/mount and `<0` to decline. It
runs synchronously in the registering driver's context (outside the block-device
lock, so it may issue block I/O). First provider to claim a device wins.

---

## `storage_register_blockdev`

- **kind:** function
- **lang:** c
- **source:** `servers/CoreStorage/src/main.c`
- **hash:** 5bc4292d3a89f134

Register a block device with CoreStorage and offer it to every registered filesystem provider.

**Parameters**

- `desc` — `storage_blockdev_t` to register; its contents are copied.
- `handle` — out (may be `NULL`); receives the opaque handle naming the
  registered device.

**Returns** `0`. The copy's `claimed` is forced to `0`, the device is appended to
the registry, then offered to each fs provider via `probe` (first to claim wins).
Because providers may do block I/O while probing, probing runs outside the
block-device lock. CoreStorage is also implemented in Lisp
(`lisp/servers/corestorage.clp`); this is the C registration ABI.

---

## `storage_unregister_blockdev`

- **kind:** function
- **lang:** c
- **source:** `servers/CoreStorage/src/main.c`
- **hash:** 2b906ab4882fcd12

Remove a previously registered block device (e.g. on USB unplug).

**Parameters**

- `handle` — the handle `storage_register_blockdev` wrote.

**Returns** `0` on success, `-1` if `handle` is `NULL` or not found. Frees the
CoreStorage-owned copy. A filesystem provider that had mounted the device is
**not** notified — providers gain an unmount hook when the FS-provider API grows
one.

---

## `storage_register_fsprovider`

- **kind:** function
- **lang:** c
- **source:** `servers/CoreStorage/src/main.c`
- **hash:** 701ba5b549d230ea

Register a filesystem provider and offer it every already-registered, unclaimed block device.

**Parameters**

- `desc` — `storage_fsprovider_t` to register; its contents are copied.

**Returns** `0`. The provider's `probe` is called for each unclaimed device
present at registration time (and, going forward, each newly registered device);
a device it claims is marked and never offered again. A provider may legitimately
claim several devices, so the loop does not stop at the first match.

---

## `storage_blockdev_count`

- **kind:** function
- **lang:** c
- **source:** `servers/CoreStorage/src/main.c`
- **hash:** ee4e7d96ad623752

Return the number of registered block devices.

**Returns** the current count (read under the block-device lock). Use with
`storage_blockdev_get` to enumerate.

---

## `storage_blockdev_get`

- **kind:** function
- **lang:** c
- **source:** `servers/CoreStorage/src/main.c`
- **hash:** dfa4a611810cf646

Return the opaque handle for the block device at a given enumeration index.

**Parameters**

- `idx` — index in `[0, storage_blockdev_count())`.

**Returns** the device handle, or `NULL` if `idx` is out of range. The index is
not stable across unregistration.

---

## `storage_blockdev_info`

- **kind:** function
- **lang:** c
- **source:** `servers/CoreStorage/src/main.c`
- **hash:** a8b3af386e391952

Return the read-only `storage_blockdev_t` descriptor for a block-device handle.

**Parameters**

- `handle` — a handle from `storage_blockdev_get`/`storage_register_blockdev`.

**Returns** the handle cast to `const storage_blockdev_t *` (the handle *is* the
descriptor); exposes name, block size/count, etc.

---

## `storage_blockdev_read`

- **kind:** function
- **lang:** c
- **source:** `servers/CoreStorage/src/main.c`
- **hash:** 53e61a29380c56b7

Read `count` blocks starting at `lba` from a registered block device into `buf`.

**Parameters**

- `handle` — the device handle.
- `lba` / `count` — starting block and block count.
- `buf` — destination buffer (`count * block_size` bytes).

**Returns** the driver's `read` result, or `-1` if `handle`/its `read` is `NULL`
or `lba + count` exceeds `block_count` (bounds-checked here before dispatch).

---

## `storage_blockdev_write`

- **kind:** function
- **lang:** c
- **source:** `servers/CoreStorage/src/main.c`
- **hash:** bffdd9ca969be9dd

Write `count` blocks starting at `lba` to a registered block device from `buf`.

**Parameters**

- `handle` — the device handle.
- `lba` / `count` — starting block and block count.
- `buf` — source buffer (`count * block_size` bytes).

**Returns** the driver's `write` result, or `-1` if `handle`/its `write` is
`NULL` or `lba + count` exceeds `block_count`.

---

## `pwr_device_t`

- **kind:** struct
- **lang:** c
- **source:** `servers/inc/CorePower/power.h`
- **hash:** b36d7a535898fad2

A power-managed device registered with CorePower: a 16-char name, global/device state-change event handlers, current P/D/G state, and the device's power class.

`event_g(tgt_state, p_state)` handles global (system) power-state transitions;
`event_d(tgt_state)` handles per-device transitions. `dev_class` is a
`device_pwr_class_t` bitmask used to target events. The descriptor is enqueued by
pointer, so it must remain valid for the device's lifetime.

---

## `global_pwr_state_t`

- **kind:** enum
- **lang:** c
- **source:** `servers/inc/CorePower/power.h`
- **hash:** 540b6e9e7d2099dd

ACPI-style global/system power states: `g0_pXX` (on, with a performance state), `g1_s1`/`g1_s2`/`g1_s3`/`g1_s4` (suspend/sleep/hibernate), `g2` (soft off).

---

## `device_pwr_state_t`

- **kind:** enum
- **lang:** c
- **source:** `servers/inc/CorePower/power.h`
- **hash:** ad3920b203291c6e

ACPI-style per-device power states: `d0` (full on), `d1`/`d2` (device-defined), `d3` (full off).

---

## `device_pwr_class_t`

- **kind:** enum
- **lang:** c
- **source:** `servers/inc/CorePower/power.h`
- **hash:** 489860bbb1949e33

Power-class bit flags identifying/targeting devices: `generic`, `display`, `audio_out`, `audio_in`, `human_interface_device`, `camera`, `processor`.

Used both in `pwr_device_t.dev_class` and as the `pwr_class` selector of
`pwr_sendevent_g`/`pwr_sendevent_d`, which deliver an event to every registered
device whose class intersects the mask.

---

## `pwr_register`

- **kind:** function
- **lang:** c
- **source:** `servers/CorePower/src/main.c`
- **hash:** 50611d1f847d7e94

Register a power-managed device with CorePower.

**Parameters**

- `device` — caller-owned `pwr_device_t`; enqueued by pointer.

**Returns** `0` on success, `-1` if the device queue is full. CorePower is also
implemented in Lisp (`lisp/servers/corepower.clp`); this is the C registration
ABI.

---

## `pwr_sendevent_g`

- **kind:** function
- **lang:** c
- **source:** `servers/CorePower/src/main.c`
- **hash:** cf309e018325e3a0

Deliver a global (system) power-state change event to all registered devices in a class.

**Parameters**

- `pwr_class` — `device_pwr_class_t` mask; a device receives the event if its
  `dev_class` intersects this mask.
- `state` — target `global_pwr_state_t`.
- `p_state` — target performance state (meaningful for `g0_pXX`).

**Returns** `0`. Each matching device's `event_g(state, p_state)` is invoked (if
non-NULL) while the device queue lock is held.

---

## `pwr_sendevent_d`

- **kind:** function
- **lang:** c
- **source:** `servers/CorePower/src/main.c`
- **hash:** a208656c8e7dea66

Deliver a per-device power-state change event to all registered devices in a class.

**Parameters**

- `pwr_class` — `device_pwr_class_t` mask selecting target devices.
- `state` — target `device_pwr_state_t`.

**Returns** `0`. Each matching device's `event_d(state)` is invoked (if non-NULL)
while the device queue lock is held.
