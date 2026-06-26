# usb-audio

> USB Audio Class 1 (UAC1) playback driver: registers with coreusb as the class handler for audio devices, probes for an isochronous OUT endpoint, and exposes a coreaudio card that streams synthesized PCM over USB isochronous transfers.

| | |
|---|---|
| **Source** | `lisp/drivers/usb-audio.clp` |
| **Kind** | driver |
| **Bound by** | `lisp/init.clp` — `usb-audio-init` is called before any host controller is initialized; actual device binding happens when coreusb dispatches a `probe` message for a `USB-CLASS-AUDIO` (`#x01`) device |
| **Registers with** | [coreusb](../servers/coreusb.md) via `register-class` (class `#x01`); [coreaudio](../servers/coreaudio.md) via `register` on each claimed device |
| **Capabilities** | imports `coreusb` (descriptor accessors, HCI transfer protocol) and `driver-util` (serve, spawn-restricted, nth) |

## Overview

usb-audio is a two-layer driver. The outer layer is a persistent class-driver dispatcher context returned by `usb-audio-init`; it receives `probe`/`remove` messages from coreusb and manages a list of claimed devices. The inner layer is one dedicated card context per claimed device, spawned by `start-audio-card`; it handles the full lifetime of a single USB audio device — isochronous streaming, coreaudio request serving, and hot-remove teardown.

Scope is deliberately narrow to match the isochronous engine: **playback only**, fixed at **48 kHz stereo 16-bit** (192 bytes/ms, 4 bytes/frame), square-wave synthesis. The PCM synthesis mirrors the `hdaudio` driver exactly so both card types sound identical at the REPL. Volume/mute (UAC Feature Units) and capture are not wired up.

The card context's stop-handling design is noteworthy: the internal `await` loop does not use the coreusb `await-complete` wrapper. Instead it watches for `stop` alongside `complete`, sets a flag when `stop` arrives mid-transfer, and lets the in-flight chunk finish before aborting the play loop and exiting. This ensures clean teardown on hot-remove without orphaning the context.

## Initialization

`init.clp` calls `usb-audio-init` with the coreusb and coreaudio handles, **before** any host controller is started:

```scheme
(usb-audio-init usb audio)
; usb   — handle returned by (start-usb-service)
; audio — handle returned by (start-audio-service)
; → class-driver dispatcher context (also registered with coreusb as the audio class handler)
```

Internally this spawns a `serve` loop that holds state `(next-id devs)` — `next-id` is a monotonically advancing counter used to generate card names (`usbaudio0`, `usbaudio1`, …); it advances **only** when a device is actually claimed, so non-audio probes do not consume names. `devs` is an alist of `(usb-bus-address . card-context)` pairs for all currently live cards.

After spawning the dispatcher the function sends:

```scheme
(send usb (list 'register-class USB-CLASS-AUDIO ctx))
```

so that coreusb routes all future audio-device `probe`/`remove` events to it.

## Class-driver dispatch protocol

The dispatcher context receives these messages from coreusb. No reply is expected for either.

### `probe`

- **Request:** `('probe dev)` — `dev` is the enumerated-device record from coreusb (see [coreusb § Enumerated-device record](../servers/coreusb.md)).
- **Behavior:** calls `find-iso-out` on `dev`. If no AudioStreaming interface with an isochronous OUT endpoint is found, the device is rejected (logged, not claimed). Otherwise, `start-audio-card` is called, a new card context is spawned, and the entry `(addr . ctx)` is prepended to `devs`. `next-id` increments only on a successful claim.
- **Reply:** none.

### `remove`

- **Request:** `('remove addr)` — `addr` is the USB bus address of the disconnected device.
- **Behavior:** finds all entries in `devs` matching `addr`, sends `('stop)` to each matching card context (triggering its teardown), and removes those entries from `devs`. Remaining entries are preserved.
- **Reply:** none.

## Card context message protocol

Each card context is a `spawn-restricted` loop that receives messages from coreaudio (forwarded by coreaudio from clients). The context handles these tags:

### `tone`

Replay the built-in bring-up tone (500 Hz square wave, amplitude 8000, 4800 frames = 0.1 s).

- **Request:** `('tone)` — no payload.
- **Reply:** none (fire-and-forget; synthesis blocks the card context until the stream finishes).

### `play`

Play a square-wave tone at arbitrary parameters.

- **Request:** `('play freq amp frames)`
  - `freq` — frequency in Hz (integer, must be > 0)
  - `amp` — peak amplitude (integer, must be > 0 and < 32768; values outside this range are rejected with a log message and the request is ignored)
  - `frames` — duration in PCM frames at 48 kHz (integer, must be > 0; 4800 = 0.1 s)
- **Reply:** none.
- **Errors:** if any parameter fails the range check, the message is logged and silently dropped; the serve loop continues.

### `endpoints`

Return the card's endpoint descriptor list.

- **Request:** `('endpoints reply)` — `reply` is the caller's context handle.
- **Reply:** sent to `reply` — always `((0 out speaker #t))` (one output, always-present speaker; no jack-sense polling).

### `get-volume`

Query endpoint volume. **Stub — no UAC Feature Unit is wired.**

- **Request:** `('get-volume ep-id reply)` — `ep-id` is the endpoint id fixnum; `reply` is the caller's context handle.
- **Reply:** sent to `reply` — always `#f`.

### `set-volume`

Set endpoint volume. **Stub — accepted and ignored.**

- **Request:** `('set-volume ep-id vol)`.
- **Reply:** none. The message is consumed and the serve loop continues without any hardware interaction.

### `mute`

Mute or unmute an endpoint. **Stub — accepted and ignored.**

- **Request:** `('mute ep-id on?)`.
- **Reply:** none. Consumed silently.

### `get-status`

Query card status.

- **Request:** `('get-status reply)` — `reply` is the caller's context handle.
- **Reply:** sent to `reply` — the symbol `'playing` unconditionally (there is no stopped/paused distinction while the context is alive).

### `stop`

Hot-remove signal sent by the class dispatcher on device disconnect.

- **Request:** `('stop)`.
- **Behavior:** the card sends `SET_INTERFACE 0` (zero-bandwidth alt) to the device, logs `"[usb-audio] stopped"`, and exits the serve loop. The context terminates.
- **Reply:** none.
- **Mid-stream behavior:** if `stop` arrives while a `play`/`tone` is in flight, the internal `await` sets the `stopped` flag. The in-flight isochronous submission completes normally; the play loop detects the flag at the top of its next iteration and aborts without issuing further transfers. The serve loop then detects `stopped` before its first `recv` and performs the `SET_INTERFACE 0` + exit.

## Exported functions

### `(usb-audio-init usb audio)`

The sole export. Spawns the class-driver dispatcher and registers it with coreusb.

- `usb` — coreusb service handle (from `start-usb-service`).
- `audio` — coreaudio service handle (from `start-audio-service`).
- Returns the dispatcher context handle (also registered with coreusb as the handler for `USB-CLASS-AUDIO`).
- Side effect: sends `('register-class USB-CLASS-AUDIO ctx)` to `usb`.

## Internal details

### Interface and endpoint discovery

`find-iso-out` walks `(usb-interfaces dev)` looking for the first interface record whose `iface-subclass` equals `AUDIO-SUBCLASS-STREAMING` (2) and whose endpoint list (from `usb-iface-endpoints`) contains an isochronous OUT endpoint (via `usb-find-ep-in` with `USB-XFER-ISOCH` and direction `#f`). Alt 0 of an AudioStreaming interface carries no endpoints by the UAC1 spec (zero-bandwidth setting); the active alternate setting is the one with the isochronous endpoint.

```scheme
(find-iso-out dev)
; → (interface-number alt-setting endpoint-address max-packet-size)
;    or #f if no suitable streaming interface is found
```

If `ep-max-packet` from the descriptor is 0 (should not happen on a compliant device), `find-iso-out` substitutes 192 (the canonical 48 kHz / 1 ms / stereo-16 packet size).

### Bring-up sequence

After being spawned, the card context executes in order:

1. `SET_INTERFACE iface alt` — activates the streaming alternate setting (exposes the isochronous endpoint).
2. UAC1 `SET_CUR(SAMPLING_FREQ_CONTROL)` on the endpoint — sends the 3-byte little-endian rate `SAMPLE-RATE` (48000). A device that does not implement the control replies with STALL; this is treated as harmless and the device keeps its default rate. QEMU's `usb-audio` device STALLs this control and defaults to 48 kHz.
3. `(send audio (list 'register name (self) (audio-endpoint-descs)))` — announces the card to coreaudio.
4. Plays the bring-up tone (500 Hz, 0.1 s) as an end-to-end smoke test of the isochronous path.
5. Enters the `serve-loop`.

### Isochronous streaming

PCM synthesis and submission are interleaved in chunks to avoid large allocations:

```scheme
(define CHUNK-BYTES-TARGET 1536)  ; upper bound; keeps submissions under both UHCI and xHCI iso caps
```

For a given `(freq amp frames)`:

- Packets per chunk: `floor(CHUNK-BYTES-TARGET / mps)`, minimum 1.
- Frames per chunk: `floor((packets-per-chunk * mps) / FRAME-SZ)`, minimum 1.
- Each chunk allocates a fresh `(make-bytes (* n FRAME-SZ))` buffer, fills it with a square wave (phase preserved across chunks via the `done` counter), and submits it via the internal `isoch-out` helper.

The `isoch-out` helper sends the isochronous transfer request **directly** to the HCI context, bypassing the coreusb `usb-isoch-out` wrapper:

```scheme
(send (usb-dev-hci dev)
      (list 'isoch
            (usb-dev-address dev)
            (usb-dev-speed  dev)
            ep-addr mps data len
            #f          ; dir-in? = #f (OUT)
            (self)))    ; reply target
```

It then calls the local `await` (not `await-complete`) to receive the `('complete n data)` reply while also handling an interleaved `('stop)` message.

Square-wave synthesis matches the `hdaudio` `fill-tone!` pattern: the amplitude alternates between `+amp` and `(- 65536 amp)` (unsigned 16-bit two's complement) every `floor(SAMPLE-RATE / (2 * freq))` frames. Both channels (left and right) carry the same value, written as two consecutive `bytes-u16-set!` calls.

### Card naming

Card names are generated as `(string->symbol (string-append "usbaudio" (number->string n)))` where `n` is the dispatcher's `next-id` counter. The counter advances **only** when `find-iso-out` succeeds and a card context is spawned, so names are compact even when non-audio or incompatible USB devices are probed.

## Constants

```scheme
SAMPLE-RATE          ; 48000   — fixed playback rate (Hz)
FRAME-SZ             ; 4       — bytes per PCM frame (2 channels × 2 bytes)
TONE-HZ              ; 500     — bring-up tone frequency
TONE-FRAMES          ; 4800    — bring-up tone duration (0.1 s at 48 kHz)
TONE-AMP             ; 8000    — bring-up tone amplitude (~1/4 full scale)
CHUNK-BYTES-TARGET   ; 1536    — target isochronous submission size (bytes)

AUDIO-SUBCLASS-STREAMING    ; 2     — bInterfaceSubClass value for AudioStreaming
UAC-SET-CUR                 ; #x01  — UAC class-specific SET_CUR request
UAC-SAMPLING-FREQ-CONTROL   ; #x01  — UAC sampling frequency control selector
```

## Notes / gotchas

**Volume/mute not implemented.** The UAC Feature Unit is not wired. `get-volume` always replies `#f`; `set-volume` and `mute` are silently consumed. Clients that send these messages will not wedge (the messages are handled), but no hardware effect occurs.

**Capture not supported.** Only the isochronous OUT path is implemented. There is no isochronous IN endpoint scanning, no record ring, and no capture messages in the serve loop. The `capture-start` / `capture-stop` / `capture-read` / `capture-pos` messages forwarded by coreaudio are silently dropped by the `else` clause of the card's `cond`.

**Stop-aware `await`, not `await-complete`.** The card context does **not** use the coreusb `usb-isoch-out` wrapper or `await-complete`. Using those would silently drop a `stop` message that arrives mid-transfer, leaving the context alive after hot-remove. The local `await` explicitly handles `('stop)` by setting a flag and looping back to wait for the `('complete ...)` reply, so the in-flight DMA buffer is always drained before teardown.

**`SET_INTERFACE 0` on stop.** Before exiting, the card sends `SET_INTERFACE 0` to return the streaming interface to its zero-bandwidth alternate setting. This releases the isochronous bandwidth reservation on the host controller. If the stop is due to physical disconnect, the control transfer will fail (STALL or timeout); this is benign — the `ctl-out` path calls `await`, which returns a `complete` with negative `n`, and the context exits anyway.

**SET_CUR STALL is benign.** QEMU's `usb-audio` STALLs the sampling-frequency control; the card notes the negative completion count and continues at the device's default rate (48 kHz). Real hardware that supports the control will ACK it. In either case, the bring-up tone plays at 48 kHz.

**Single speaker endpoint, always present.** `(audio-endpoint-descs)` always returns `((0 out speaker #t))`. There is no jack-sense polling, so `present` is always `#t` regardless of actual connection state.

**`get-status` always returns `'playing`.** The card has no paused or idle state; it is either streaming a tone or blocked in `recv`. The `'playing` reply is a placeholder; clients should not depend on it for flow control.

**Config descriptor cap (inherited from coreusb).** The enumerator clips config descriptors at 512 bytes. USB audio devices with many interfaces (e.g. multiple streaming interfaces for different sample rates) may have descriptors larger than this; interfaces past byte 512 are invisible to `usb-interfaces` and `find-iso-out` will only see the truncated set.

**Class-driver registration order.** `usb-audio-init` must be called before any host controller is initialized. See [coreusb § Notes / gotchas](../servers/coreusb.md) for the ordering rule and what happens if it is violated.
