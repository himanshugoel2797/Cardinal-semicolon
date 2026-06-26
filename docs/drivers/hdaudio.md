# hdaudio

> Intel HD Audio controller driver: resets the controller, stands up CORB/RIRB command rings, walks the codec graph to discover endpoints (speaker, headphone, line-out, mic, line-in), programs output/capture paths, manages per-endpoint volume/mute, polls jack presence, and handles codec hotplug — registering each card with [coreaudio](../servers/coreaudio.md).

| | |
|---|---|
| **Source** | `lisp/drivers/hdaudio.clp` |
| **Kind** | driver |
| **Bound by** | `lisp/init.clp` — iterates `(pci-find-class-all #x04 #x03)`; one `hdaudio-init` call per controller found |
| **Registers with** | [coreaudio](../servers/coreaudio.md) via `(register name (self) endpoint-descs)` |
| **Capabilities** | `sys-mmio` (`mmio-map`, `dma-alloc-32`, `bytes-phys`), `sys-pci` (`pci-find-class`, `pci-assign-bars`, `pci-setup-msi`, `msi-count`, `msi-wait`), `driver-util` (`wait-until`, `make-cell`, `cell-ref`, `cell-set!`, `bar-base`, `nth`, `copy-bytes`, `pci-enable-mem-bus-master!`) |

## Overview

`hdaudio` matches **any** PCI device with base class `0x04` (multimedia) and subclass `0x03` (HD Audio), so it binds QEMU's `intel-hda` (8086:2668) and `ich9-intel-hda` (8086:293e) as well as real Intel, NVIDIA, AMD, and VIA HDA controllers without a VID/DID table. `init.clp` calls `pci-find-class-all` to find every controller and invokes `hdaudio-init` once per device, naming them `hda0`, `hda1`, … in discovery order.

Each controller instance:

1. Maps the MMIO register block from BAR0 (calling `pci-assign-bars` if firmware left it unconfigured).
2. Resets the controller and waits for CRST deassert.
3. Allocates a single sub-4 GB DMA buffer holding the CORB (command outbound ring) and RIRB (response inbound ring), sizes them from the capability register, and starts both DMA engines.
4. Probes every codec address (0–14) via a vendor-id verb; for each present codec, walks the audio function group to discover and classify all pin complexes (endpoint enumeration).
5. Configures every output endpoint's DAC → pin path and plays a bring-up tone on the primary output.
6. Registers the endpoint set with [coreaudio](../servers/coreaudio.md).
7. Starts two background contexts: a **codec-change watcher** (parks on the controller MSI; sends `(codec-change)` when `STATESTS` changes) and a **jack-presence poller** (sends `(jack-poll)` once per second).
8. Enters `hda-driver-loop` — the card's long-lived service loop — which processes messages from coreaudio and the two helper contexts.

Reset and ring setup run in a `spawn-restricted` context so the `wait-until` settle delays yield the scheduler rather than busy-spinning (same pattern as `ahci-init`). `hdaudio-init` itself returns `'hdaudio-spawned` immediately (fire-and-forget).

## Initialization

`init.clp` calls:

```scheme
(hdaudio-init audio name ecam)
```

- `audio` — the [coreaudio](../servers/coreaudio.md) service handle.
- `name` — a symbol identifying this card (`'hda0`, `'hda1`, …).
- `ecam` — the PCI ECAM pointer returned by `pci-find-class-all` for this device, or `#f` if no device was found (in which case the call logs `"[hdaudio] no device present"` and returns `#f`).

Returns `'hdaudio-spawned` on success (bring-up continues asynchronously) or `#f` if BAR mapping fails.

The internal bring-up thunk `hdaudio-bringup` (not exported) is the body that runs inside the spawned context. On success it calls:

```scheme
(send audio (list 'register name (self) (endpoint-descs eps)))
```

registering the card name, the driver loop's context handle (`(self)`), and the plain-data endpoint descriptor list with coreaudio.

## CORB / RIRB verb interface

The HD Audio command mechanism is a pair of DMA rings. The driver allocates them from a single `dma-alloc-32` buffer (physical address < 4 GB; CORB at offset 0, RIRB after it) and programs the controller's base registers.

### Ring sizing

```scheme
(ring-entcnt szcap)   ; → 256 | 16 | 2 (from CORBSIZE/RIRBSIZE capability nibble)
(ring-sizefield entcnt)  ; → 2 | 1 | 0 (value for bits0-1 of the SIZE register)
```

The effective slot count is `(min corb-ent rirb-ent)` to keep CORB and RIRB indices in step.

### Sending a verb (polled)

```scheme
(hda-verb! regs ring rirb-off entcnt st addr node payload) ; → response-u32 | #f
```

Writes the 32-bit verb `(addr<<28 | (node & 0x7F)<<20 | (payload & 0xFFFFF))` into the next CORB slot, bumps `CORBWP`, then polls `RIRBWP` until the expected RIRB slot appears. Returns the 32-bit response word or `#f` on timeout (1 ms).

The watcher context is the **sole** `msi-wait` caller on the HDA MSI handle. `hda-verb!` deliberately polls `RIRBWP` via `wait-until` (not `msi-wait`) to avoid stealing the watcher's parked slot.

### Codec handle

```scheme
(make-cdc regs ring rirb-off slot-cnt st addr) ; → (lambda (node payload) → response)
```

A codec handle is a closure over all the MMIO/ring state plus the codec address. Pass it instead of threading six arguments through every graph-walk function. Test code can supply a mock lambda in place of a real codec handle to drive `enumerate-endpoints` without hardware.

```scheme
(cdc-verb  cdc node payload) ; raw verb
(cdc-param cdc node param)   ; GET_PARAMETER shorthand (verb payload = 0xF0000 | param)
```

### Verb payload helpers

```scheme
(v12 verb-id data)   ; 12-bit verb + 8-bit data  → 20-bit payload
(v4  verb-id data)   ; 4-bit verb  + 16-bit data → 20-bit payload
```

## Codec graph walk

### Function group discovery

```scheme
(find-afg cdc)   ; → audio-function-group nid | #f
```

Scans the codec root node's children for the first node whose `FUNC_GRP_TYPE` low-7 bits equal 1 (audio function group).

### Widget introspection

```scheme
(widget-type  cdc nid)           ; → widget-type field (bits20-23 of AUDIO_WIDGET_CAPS) | -1
(node-children cdc nid)          ; → (start . count) | #f
(conn-entries  cdc node)         ; → list of connected nids (short-form only; warns on long-form)
(conn-index    cdc node target)  ; → index of target in connection list | #f
```

Widget type constants: `WIDGET-AUDIO-OUTPUT` (0), `WIDGET-AUDIO-INPUT` (1), `WIDGET-MIXER` (2), `WIDGET-SELECTOR` (3), `WIDGET-PIN-COMPLEX` (4).

### Converter resolution

Output pins (DAC side):

```scheme
(find-conv      cdc node want-type depth)      ; follow conn list to a converter
(find-conv-list cdc es   want-type depth)      ; (sibling for mutual recursion)
(resolve-output-conv cdc pin)                  ; → DAC nid | #f
```

Input pins (ADC side) — scans all widgets in the AFG for an ADC whose graph reaches the pin:

```scheme
(reaches?      cdc node target depth)           ; #t if target reachable
(reaches-list? cdc es   target depth)           ; (sibling for mutual recursion)
(resolve-input-conv cdc afg-start afg-cnt pin)  ; → ADC nid | #f
```

Maximum graph traversal depth is `GRAPH-DEPTH` (4), covering pin → up-to-3 mixers/selectors → converter.

**Implementation note:** `find-conv`/`find-conv-list` and `reaches?`/`reaches-list?` are paired top-level `define`s rather than a single named-let. This is required because the bytecode VM cannot capture a top-level `define`'s own name from inside its own body's named-let; mutual recursion must be expressed as sibling globals.

### Pin configuration decoding

```scheme
(cfg-device       cfg)   ; bits20-23 → device code
(cfg-connectivity cfg)   ; bits30-31 → 0=jack 1=none(dead) 2=fixed 3=both
(dev-name code)          ; device code → 'speaker | 'headphone | 'line-out | 'line-in | 'mic | ...
```

Recognised `dev-name` symbols: `'line-out` (0x0), `'speaker` (0x1), `'headphone` (0x2), `'spdif-out` (0x4), `'digital-out` (0x5), `'line-in` (0x8), `'aux` (0x9), `'mic` (0xA), `'spdif-in` (0xC), `'digital-in` (0xD), `'other` (anything else).

## Endpoint model

### Endpoint record

Each usable pin complex becomes one or two endpoints (one per supported direction). Endpoints are mutable vectors with 9 fields:

| Index | Accessor | Type | Meaning |
|-------|----------|------|---------|
| 0 | `ep-id` | fixnum | globally-unique id across codecs on this card |
| 1 | `ep-dir` | symbol | `'out` or `'in` |
| 2 | `ep-dev` | symbol | `'speaker`, `'headphone`, `'line-out`, `'mic`, `'line-in`, … |
| 3 | `ep-pin` | fixnum | pin complex nid |
| 4 | `ep-conv` | fixnum | DAC nid (output) or ADC nid (input) |
| 5 | `ep-afg` | fixnum | audio function group nid |
| 6 | `ep-present` | bool | jack presence at last poll |
| 7 | `ep-vol` | fixnum | stored volume (0–100, default 100) |
| 8 | `ep-cdc` | closure | codec handle; not sent across context boundaries |

```scheme
(mk-endpoint id dir dev pin conv afg present cdc)   ; → endpoint vector
```

Mutators: `(ep-present! e v)`, `(ep-vol! e v)`.

### Endpoint enumeration

```scheme
(enumerate-endpoints cdc start-id)   ; → list of endpoint vectors
```

Walks every pin complex in the AFG. Skips pins with `CONN-NONE` connectivity or no resolved converter. A combo jack capable of both directions yields two endpoints (out first, then in). Endpoint ids are assigned consecutively from `start-id`, so multi-codec cards (each codec calls `enumerate-endpoints` with the running counter) produce globally unique ids.

### Endpoint descriptors (cross-context view)

```scheme
(ep-desc e)            ; → (id dir dev present) — plain data, safe to send
(endpoint-descs eps)   ; → list of ep-desc results
```

Only fixnums and symbols; the codec closure (`ep-cdc`, field 8) is NOT included. This is the form sent to coreaudio on registration and returned on `(endpoints reply)` requests.

### Primary output selection

```scheme
(primary-output eps)   ; → endpoint | #f
```

Prefers `'speaker`, then `'line-out`, then `'headphone`, then the first `'out` endpoint found.

### Endpoint lookup

```scheme
(ep-by-id eps id)   ; → endpoint | #f
```

## Output path configuration

```scheme
(configure-output! ep)
```

Powers the AFG, DAC, and pin to D0; programs the DAC's format (`FMT-48K-16-STEREO` = `#x0011`, 48 kHz 16-bit stereo), stream number (`STREAM-NUM` = 1) and channel 0; unmutes the DAC output amp to max gain; routes the pin's selector to the DAC (only if the DAC appears in the pin's connection list); enables the pin's output drive (bit 6) and headphone amp (bit 7); unmutes the pin amp; and asserts EAPD (external amp enable, BTL bit 1). Called by `scan-all-codecs` for every output endpoint at bring-up and after codec-change rescans.

## Stream / BDL setup

### Stream descriptors

Output stream 0 is at MMIO offset `0x80 + iss * 0x20` where `iss` is the input-stream count from `GCAP` bits 8–11. Input stream 0 is at `0x80`.

```scheme
(out-sd-base regs)   ; → MMIO byte offset of output stream descriptor 0
(in-sd-base  regs)   ; → MMIO byte offset of input stream descriptor 0 (always 0x80)
```

### BDL entry

```scheme
(bdl-entry! bdl eoff phys len)
```

Writes one 16-byte Buffer Descriptor List entry `{u64 addr; u32 len; u32 ioc=1}` at byte offset `eoff` in `bdl`. IOC is always set so each buffer completion raises an interrupt.

### Start / stop

```scheme
(stream-run! regs sd bdl buf total-bytes stream-num)
```

Builds a two-entry BDL splitting `buf` at the midpoint (the spec requires ≥ 2 entries), resets the stream descriptor (SRST handshake), programs BDL base, `SDCBL`, `SDLVI`, `SDFMT`, the stream-number nibble in `SDCTL+2`, and sets `RUN`. The DMA engine cycles the BDL continuously; an output buffer loops forever, an input buffer is a continuously-overwritten capture ring.

```scheme
(stream-stop! regs sd)
```

Clears `RUN` and waits for the controller to acknowledge it before returning (guards against a DMA write after the caller drops the buffer).

### Tone synthesis and playback

```scheme
(play-tone! regs freq amp nframes)   ; → (list buf bdl)
```

Synthesises a square wave at `freq` Hz (integer arithmetic, no FP) into a 32-bit DMA buffer (`nframes * 4` bytes, stereo 16-bit), then calls `stream-run!` on output stream 0. Default values: `TONE-HZ` = 500 Hz, `TONE-AMP` = 8000 (~¼ full scale), `TONE-FRAMES` = 4800 (0.1 s, clean loop at 48 kHz).

The caller **must** retain the returned `(buf bdl)` list for as long as the stream runs; the DMA engine references `buf` physically and the GC must not reclaim it.

Called automatically at the end of `hdaudio-bringup` if a primary output endpoint exists.

## Capture (input stream)

```scheme
(configure-input! ep stream-num)
```

Powers the AFG, ADC, and pin to D0; sets pin control to `IN_EN` (bit 5); routes the ADC's selector to the pin; programs the ADC format and stream tag; unmutes the ADC and pin input amps at max gain.

```scheme
(capture-start! regs ep)   ; → (list buf bdl nbytes)
```

Calls `configure-input!` with `CAPTURE-STREAM` (= 2), allocates a `CAP-FRAMES * 4`-byte (4800 frames = 0.1 s at 48 kHz stereo 16-bit) cyclic DMA ring, and starts input stream 0 via `stream-run!`. Returns `(list buf bdl nbytes)` — the caller must retain this triple.

```scheme
(capture-pos regs)   ; → SDLPIB of input stream 0 (bytes written mod CBL)
```

On QEMU with a `wav` audiodev that has no source the DMA engine still runs (LPIB advances) but the buffer contains silence.

## Per-endpoint volume and mute

Volume is a 0–100 percentage mapped onto the widget's advertised amp step range. The target widget is the pin's own amp if it has one in the endpoint direction (independent volume for speaker and headphone jack on a shared DAC), otherwise the converter's amp.

```scheme
(set-endpoint-volume! ep vol)   ; → clamped vol (0-100); vol=0 mutes the amp
(set-endpoint-mute!   ep on?)   ; mute/unmute without losing stored volume
(ep-vol ep)                     ; → stored volume (0-100)
```

`set-endpoint-volume!` clamps `vol` to 0–100, maps it linearly onto the amp's max step count (`amp-max-gain`), issues `SET_AMP_GAIN_MUTE` (4-bit verb, 16-bit payload), and stores the clamped value in the endpoint's `ep-vol` field. Volume 0 sets the mute bit.

`set-endpoint-mute!` re-issues the amp verb at the endpoint's current stored gain with the mute bit set or cleared, so a subsequent unmute restores the previous level.

Amp selection logic:

```scheme
(widget-has-amp? cdc nid dir)   ; #t if AUDIO_WIDGET_CAPS bit2 (out) or bit1 (in) is set
(ep-amp-target ep)              ; → pin nid if it has the right-direction amp, else conv nid
```

## Jack-presence detection

```scheme
(read-present cdc pin)   ; → #t (present) | #f; timeout treated as present
```

Issues `GET_PIN_SENSE` (verb `0xF09`). Bit 31 of the response = 1 means presence detected. A verb timeout returns `#t` (conservative: never spuriously marks a working jack absent). Only voltage-presence jacks are handled; resistance-sense jacks need an `EXECUTE_PIN_SENSE` (0xF08) trigger first, which is not currently issued.

```scheme
(poll-jacks! eps)   ; → list of (id dev present) for every endpoint whose presence changed
```

Re-reads pin sense for every endpoint. For each change, updates `ep-present!` in place, logs the event, and accumulates `(id dev present)` into the returned change list.

```scheme
(start-jack-poller drvloop)
```

Spawns a restricted context that loops forever: `(sleep JACK-POLL-NS)` (1 second = `1_000_000_000` ns) then `(send drvloop '(jack-poll))`. The poller never touches MMIO; only the driver loop does, preventing concurrent RIRB access.

## Codec hotplug

```scheme
(scan-all-codecs regs ring rirb-off slot-cnt st)   ; → endpoint list (all codecs)
```

Probes all 15 possible codec addresses (0–14) with a `GET_PARAMETER VENDOR_DEVICE_ID` verb. A non-zero, non-`#f` response means a codec is present. For each present codec, calls `enumerate-endpoints` (ids continue from the running counter) and `configure-output!` on every discovered output endpoint. Returns the combined endpoint list across all codecs.

This is the single reconciliation point: called at bring-up and again on every `(codec-change)` message from the watcher.

```scheme
(start-codec-watcher regs msi drvloop)
```

Spawns a restricted context. Each iteration:
1. Reads `STATESTS`. If non-zero, writes it back (W1C) to clear the sticky bits, then sends `(list 'codec-change)` to the driver loop.
2. Parks on `(msi-wait msi seen)` until the MSI fires again.

The watcher checks `STATESTS` at the **top** of each iteration (including before the first park) to catch codecs that hot-add during the boot window. It is the **sole** `msi-wait` caller on this card's MSI handle (see the [verb interface](#sending-a-verb-polled) and [Notes / gotchas](#notes-gotchas) for why `hda-verb!` polls the RIRB instead).

On `(codec-change)`, the driver loop stops any active capture stream (the old ADC endpoint may be gone), calls the `rescan` thunk (`scan-all-codecs` + log), and continues with the fresh endpoint set. The output stream is **not** stopped; the output BDL/buffer is retained from `cur`.

## Multi-controller naming (hda0 / hda1 / …)

`init.clp` iterates `(pci-find-class-all #x04 #x03)` and calls:

```scheme
(hdaudio-init audio (string->symbol (string-append "hda" (number->string idx))) ecam)
```

for each result, incrementing `idx`. Each controller gets an independent spawned context, DMA ring, endpoint list, driver loop, codec watcher, and jack poller. The name (e.g. `'hda0`) is sent verbatim to coreaudio in the `:register` message and appears in all log lines.

## Message protocol

The driver loop (`hda-driver-loop`) is the running context clients talk to via `send`. Coreaudio forwards messages from its clients.

### `(tone)`

Play the default bring-up tone (500 Hz square wave, 0.1 s, stream 1) on the primary output. Replaces the current output DMA reference.

### `(play freq amp frames)`

Play a custom square wave. `freq` must be > 0; `amp` in [1, 32767]; `frames` > 0. Bad parameters are silently ignored (no reply). Replaces the current output DMA reference.

- **Reply:** none.
- **Errors:** invalid params → logged and dropped.

### `(endpoints reply)`

- **Reply:** sends `(endpoint-descs eps)` to `reply` — a list of `(id dir dev present)` tuples for all currently-known endpoints.

### `(set-volume ep-id vol)`

Set the volume of endpoint `ep-id` to `vol` (0–100). Volume 0 mutes. Unknown `ep-id` is logged and ignored.

- **Reply:** none.

### `(get-volume ep-id reply)`

- **Reply:** sends the stored volume (0–100) to `reply`, or `#f` if `ep-id` is unknown.

### `(mute ep-id on?)`

Mute (`on?` = `#t`) or unmute (`on?` = `#f`) endpoint `ep-id` without altering its stored volume.

- **Reply:** none.

### `(capture-start ep-id)`

Configure and start capture on input endpoint `ep-id`. Must be an `'in` endpoint. Replaces any prior `cap` state. Unknown or non-input `ep-id` is logged and ignored.

- **Reply:** none.

### `(capture-read reply)`

- **Reply:** sends a byte-copy of the current capture ring to `reply` (via `copy-bytes`), or `#f` if no capture is active.

### `(capture-pos reply)`

- **Reply:** sends the DMA position in the capture ring (bytes mod CBL) to `reply`, or `#f` if no capture is active.

### `(capture-stop)`

Stops the input stream (`stream-stop!`). Clears capture state.

- **Reply:** none.

### `(codec-change)`

Sent by the codec-change watcher when `STATESTS` is non-zero. Stops any active capture, rescans all codec addresses, updates the endpoint set.

- **Reply:** none.

### `(jack-poll)`

Sent by the jack poller every second. Calls `poll-jacks!` on the current endpoint set.

- **Reply:** none.

### `(get-status reply)`

- **Reply:** sends `'playing` (the only currently-possible status) to `reply`.

## Exported functions

These are exported from `(define-module hdaudio ...)` and available to in-OS tests via the testable-internals pattern (mock codec handles instead of real hardware).

### `(hdaudio-init audio name ecam)`

Entry point. Maps ECAM + BAR0, spawns the bring-up context. Returns `'hdaudio-spawned` or `#f`.

### `(enumerate-endpoints cdc start-id)`

Walk the codec graph for `cdc` and return a list of endpoint vectors. Pin complexes with `CONN-NONE` connectivity or no resolvable converter are omitted. Combo jacks yield two endpoints. Tests can supply a mock lambda for `cdc`.

### `(endpoint-descs eps)`

Map `ep-desc` over an endpoint list. Returns a list of `(id dir dev present)` tuples — the plain-data view suitable for sending across context boundaries.

### `(ep-desc e)`

Single-endpoint descriptor: `(list (ep-id e) (ep-dir e) (ep-dev e) (ep-present e))`.

### `(primary-output eps)`

Return the best output endpoint: speaker > line-out > headphone > first-out, or `#f`.

### `(ep-vol ep)`

Return the stored volume (0–100) for an endpoint.

### `(set-endpoint-volume! ep vol)`

Map `vol` (0–100) onto the endpoint's amp step range and issue `SET_AMP_GAIN_MUTE`. Returns the clamped volume applied.

### `(set-endpoint-mute! ep on?)`

Mute/unmute an endpoint at its current stored gain.

### `(configure-input! ep stream-num)`

Program the input path for `ep` (power, pin control, ADC routing, format, stream tag, amp). Separated from `capture-start!` so tests can call it without allocating a DMA ring.

### `(poll-jacks! eps)`

Re-read pin sense for all endpoints, update `ep-present!` on changes, and return the change list.

### `(read-present cdc pin)`

Issue `GET_PIN_SENSE` for `pin` on `cdc`. Returns `#t` (present) or `#f`; timeouts treated as present.

## Notes / gotchas

**Fire-and-forget bring-up.** `hdaudio-init` returns immediately. The spawned `hdaudio-bringup` context performs the reset, ring setup, enumeration, and coreaudio registration asynchronously. If `init.clp` queries coreaudio for endpoints immediately after `hdaudio-init` returns the card may not yet be registered.

**Single `msi-wait` caller per MSI handle.** The kernel's MSI wake bridge has one parked-waiter slot per handle. `start-codec-watcher` is the only code that calls `msi-wait` on the HDA MSI; `hda-verb!` polls `RIRBWP` via `wait-until` instead. Adding a second `msi-wait` caller would silently starve the watcher.

**RIRB slot tracking.** `hda-verb!` snapshots `RIRBWP` *before* posting the verb and derives the expected response slot as `(modulo (+ rwp 1) entcnt)`. This is correct on both spec-compliant controllers (which reset RIRBWP to 0xFF so the first response lands in slot 0) and QEMU (which resets it to 0). Do not assume CORB slot index == RIRB slot index.

**DMA buffer lifetime.** `play-tone!` and `capture-start!` return `(list buf bdl …)`. The DMA engine references `buf` (and `bdl`) by physical address forever until the stream is stopped. The Lisp GC does not know about the physical reference; the caller must keep a live Scheme reference to prevent the pages from being reclaimed and reused.

**Output stream never stopped.** `hda-driver-loop` replaces the `cur` DMA reference on each `tone`/`play` message. The old buffer becomes unreferenced and GC-eligible. The stream continues running against the new BDL; there is no stream stop/restart between tones.

**Long-form connection lists not supported.** `conn-entries` logs a warning and returns `'()` for connection lists with the long-form flag (bit 7 of `CONN_LIST_LEN`) set. Such lists appear only on codecs with > 127 widgets, which is uncommon in practice.

**Resistance-sense jacks.** `read-present` issues only `GET_PIN_SENSE` (verb `0xF09`), which is correct for voltage-presence and QEMU jacks. Codecs using impedance/resistance detection require an `EXECUTE_PIN_SENSE` (0xF08) trigger beforehand to latch a fresh measurement. This is not currently implemented.

**Jack poller wakes active sleep.** The jack poller context calls `(sleep 1_000_000_000)` then `(send drvloop ...)`. Because the kernel `sleep` shares the blocked flag with `recv`, any `send` to the poller context while it is sleeping wakes it early. In practice nothing sends to the poller, so this is benign, but it is the same hazard documented in the `corenetwork` DHCP renew loop (see `notes/AUDIT.md`).

**QEMU capture is silence.** QEMU's HDA emulation with a `wav` audiodev produces no input data; the capture DMA engine runs (LPIB advances) but the ring fills with silence. The DMA-position advancing is the proof the capture path is live.

**Unsolicited responses disabled.** The `GCTL.UNSOL` bit is not set. Codec unsolicited responses (e.g. for jack events) would interleave in the RIRB and break `hda-verb!`'s solicited-response poller. Jack detection is done by polling instead.

**Format fixed at 48 kHz / 16-bit / stereo.** `FMT-48K-16-STEREO` (`#x0011`) is the only format programmed into both stream descriptors and converter format registers. Sample-rate negotiation, multi-channel, and 24/32-bit formats are not implemented.

**Stream descriptors assume iss ≤ 4.** The MMIO window is mapped as one page (`0x1000`). A controller with many streams places output descriptors at `0x80 + iss*0x20`; for iss > 15 the descriptor would fall outside the page. In practice no HDA controller has more than 15 input streams, but the comment in `hdaudio-bringup` notes this as the reason a full page is mapped rather than just `0x180`.
