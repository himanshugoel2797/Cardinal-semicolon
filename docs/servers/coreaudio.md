# coreaudio

> Front-door audio service that routes playback, capture, and control messages to registered card driver contexts.

| | |
|---|---|
| **Source** | `lisp/servers/coreaudio.clp` |
| **Kind** | server |
| **Bound by** | `lisp/init.clp` — always; started unconditionally in `system-init`, before any hardware probing |
| **Registers with** | n/a — coreaudio IS the registry; drivers register with it |
| **Capabilities** | imports `driver-util` (for `serve`, `reply-to`, `ctx?`, `nth`) |

## Overview

coreaudio is a pure routing layer: it maintains a table of registered audio cards (each a `(name . ctx)` pair) and forwards every request to the owning driver context. It never touches hardware directly — that matches the least-privilege posture of the other `Core*` servers.

The original C `CoreAudio` was a complete stub (its `module_init` only registered tests and nothing else). This Lisp replacement is fully functional once a driver registers a card.

Two driver types register with coreaudio:

- **HD Audio controllers** — `hdaudio-init` spawns one card context per controller and calls `(register name ctx endpoints)` after codec enumeration. Default card name is `hda0`, `hda1`, etc.
- **USB Audio (UAC1) devices** — `usb-audio-init` registers cards named `usbaudio0`, `usbaudio1`, etc. via the same `register` message. USB audio supports only `tone`, `play`, and `endpoints` messages (the capture/volume/mute/rescan/poll-jacks paths are not wired up in the UAC1 driver).

The service context is a `serve` loop (from `driver-util`) that accumulates the card table as its state. Unknown message tags are silently dropped, so a misbehaving client cannot crash the service.

## Initialization

`init.clp` calls `start-audio-service` unconditionally in `system-init` and stores the returned handle in `audio-service`, which is then passed to each driver that needs to register a card.

```scheme
(start-audio-service)   ; → service handle (a context ref)
```

No arguments. Spawns the `serve` loop internally; the caller receives the context handle to send messages to.

In `init.clp` the audio setup looks like:

```scheme
(let ((audio (start-audio-service)))
  (set! audio-service audio)
  ;; ... then for each detected HD Audio PCI device:
  (hdaudio-init audio 'hda0 ecam)
  ;; USB audio piggybacks the same handle:
  (usb-audio-init usb audio-service))
```

## Message protocol

All messages are sent as a list to the handle returned by `start-audio-service`. The first element is the tag symbol. `<name>` is always the card's symbol (e.g. `'hda0`, `'usbaudio0`).

### `:register`

Driver-to-service. A card driver announces itself.

- **Request:** `(register <name> <ctx> <endpoints>)`
  - `<name>` — symbol naming the card (`'hda0`, `'usbaudio0`, etc.)
  - `<ctx>` — the driver's own context handle (`(self)` inside the driver context)
  - `<endpoints>` — list of endpoint descriptors; each descriptor is `(id dir dev present)` where `id` is a fixnum, `dir` is `'in` or `'out`, `dev` is a device-type symbol (e.g. `'speaker`, `'headphone`, `'mic`, `'line-in`, `'line-out`), and `present` is a boolean
- **Reply:** none
- **Notes:** Re-registering the same name replaces the previous entry (new `cons` is prepended; old entry shadows but `card-find` returns the first match).

### `:tone`

Play the card's default bring-up tone (500 Hz square wave, 0.1 s, defined by the driver).

- **Request:** `(tone <name>)`
- **Reply:** none — fire-and-forget; the driver context handles synthesis
- **Errors:** silently dropped if `<name>` is not registered

### `:play`

Play a synthesized square-wave tone at arbitrary parameters.

- **Request:** `(play <name> <freq> <amp> <frames>)`
  - `<freq>` — frequency in Hz (integer, > 0)
  - `<amp>` — peak amplitude, 1–32767 (integer); `init.clp` defaults to 8000
  - `<frames>` — duration in PCM frames at 48 kHz (integer); 4800 = 0.1 s
- **Reply:** none — fire-and-forget
- **Errors:** silently dropped if `<name>` is not registered; the driver also range-checks arguments and logs bad params

### `:cards`

Enumerate all registered card names.

- **Request:** `(cards <reply>)`
  - `<reply>` — caller's own context handle (pass `(self)`)
- **Reply:** sent to `<reply>` — a list of card name symbols, e.g. `('hda0 'usbaudio0)`; empty list if none registered

### `:endpoints`

Retrieve live endpoint descriptors for a card. coreaudio forwards the request to the driver context (which has the live codec handle) rather than answering from the registration-time snapshot, so jack-presence changes are reflected.

- **Request:** `(endpoints <name> <reply>)`
  - `<reply>` — caller's context handle (pass `(self)`)
- **Reply:** sent to `<reply>` — list of `(id dir dev present)` tuples; `()` if `<name>` is not registered
- **Errors:** if `<reply>` fails the `ctx?` type check, the forward is silently skipped (no reply is sent); if the card is not registered, `()` is sent immediately to `<reply>`

```scheme
; Example: enumerate endpoints
(send audio-svc (list 'endpoints 'hda0 (self)))
(let ((eps (recv)))
  ;; eps => ((0 out speaker #t) (1 out headphone #f) (2 in mic #t) ...)
  ...)
```

### `:set-volume`

Set the volume on one endpoint.

- **Request:** `(set-volume <name> <ep-id> <vol>)`
  - `<ep-id>` — endpoint id fixnum (from the endpoint descriptor list)
  - `<vol>` — volume 0–100 (integer; 0 mutes)
- **Reply:** none
- **Errors:** silently dropped if `<name>` is not registered

### `:get-volume`

Query the current volume of one endpoint.

- **Request:** `(get-volume <name> <ep-id> <reply>)`
  - `<ep-id>` — endpoint id fixnum
  - `<reply>` — caller's context handle
- **Reply:** sent to `<reply>` — the volume as a fixnum 0–100, or `#f` if the card is not registered
- **Errors:** if `<reply>` fails `ctx?`, the forward is skipped

### `:mute`

Mute or unmute one endpoint.

- **Request:** `(mute <name> <ep-id> <on?>)`
  - `<ep-id>` — endpoint id fixnum
  - `<on?>` — `#t` to mute, `#f` to unmute
- **Reply:** none
- **Errors:** silently dropped if `<name>` is not registered

### `:capture-start`

Begin DMA capture on an input endpoint.

- **Request:** `(capture-start <name> <ep-id>)`
  - `<ep-id>` — id of an input (`'in`) endpoint
- **Reply:** none
- **Errors:** silently dropped if `<name>` is not registered; only meaningful for HD Audio cards (USB audio does not implement capture)

### `:capture-stop`

Stop an in-progress capture.

- **Request:** `(capture-stop <name>)`
- **Reply:** none
- **Errors:** silently dropped if `<name>` is not registered

### `:capture-read`

Read a copy of the DMA capture ring buffer.

- **Request:** `(capture-read <name> <reply>)`
  - `<reply>` — caller's context handle
- **Reply:** sent to `<reply>` — a `bytes` copy of the capture ring, or `#f` if not registered
- **Notes:** the bytes object is a copy-on-send snapshot; DMA continues after the read
- **Errors:** if `<reply>` fails `ctx?`, the forward is skipped

### `:capture-pos`

Query the current DMA write position in the capture ring.

- **Request:** `(capture-pos <name> <reply>)`
  - `<reply>` — caller's context handle
- **Reply:** sent to `<reply>` — current LPIB (link position in buffer) as a fixnum, or `#f` if not registered
- **Errors:** if `<reply>` fails `ctx?`, the forward is skipped

### `:rescan`

Force a codec re-scan on the named card (hotplug reconcile). Internally forwards a `(codec-change)` message to the driver context.

- **Request:** `(rescan <name>)`
- **Reply:** none
- **Notes:** `init.clp` sends this automatically 2.5 s after boot (via a restricted context) to catch any codec that finishes settling after the initial enumeration

### `:poll-jacks`

Force an immediate jack-presence re-poll on the named card. Internally forwards a `(jack-poll)` message to the driver context.

- **Request:** `(poll-jacks <name>)`
- **Reply:** none
- **Notes:** `init.clp` fires this periodically (1 s interval) for jack-insertion detection under QEMU, where unsolicited RIRB responses are not generated

## Exported functions

### `(start-audio-service)`

Starts the coreaudio `serve` loop in a new context and returns its handle. Called once by `init.clp`; the returned handle is the sole endpoint other contexts send messages to.

No arguments. Returns a context handle. Side effect: spawns a new restricted context.

## Notes / gotchas

**coreaudio never touches hardware.** It only holds a symbol→context map. All real work happens in the driver context. This means that if the driver context dies (e.g. due to a codec fault), subsequent `send` calls to the forwarded context silently do nothing — there is no error propagation back to the original client.

**Reply-handle validation (`ctx?`).** Messages that supply a caller-owned reply handle (`endpoints`, `get-volume`, `capture-read`, `capture-pos`) are validated with `ctx?` before forwarding. Passing a non-context value (a stale handle, a fixnum, `#f`) causes the message to be silently dropped rather than crashing the service or the driver context. Without this guard, a malformed message could forward an untrusted value to the driver and kill it.

**Card table ordering.** Cards are stored as a prepended list; `card-find` returns the first (most recently registered) entry for a given name. Re-registering the same name shadows the previous entry — the old driver context is never notified and is effectively leaked. In practice each name is registered once at boot.

**USB Audio subset.** `usb-audio` registers cards via `:register` and handles `:tone`, `:play`, and `:endpoints`. The volume, mute, capture, rescan, and poll-jacks paths are forwarded to the USB card context but are not implemented by the UAC1 driver — the driver's serve loop logs and ignores them.

**`tone` vs `play`.** `:tone` asks the driver to replay its built-in bring-up tone (frequency, amplitude, and duration are driver-defined constants). `:play` gives explicit parameters. Both are fire-and-forget: synthesis runs inside the driver context and coreaudio returns immediately after forwarding.

**Endpoint descriptors are not cached.** The `:endpoints` query is always forwarded to the driver context, never answered from the registration-time `<endpoints>` argument, so the `present` field reflects live jack state (updated by periodic `:poll-jacks` or after `:rescan`).

**REPL integration.** `init.clp` exports `play-tone` and `set-vol` as REPL commands that send `:play`/`:tone` and `:set-volume` to the stored `audio-service` handle targeting `'hda0`. These commands guard their arguments with `integer?` before forwarding, since a bad type reaching the driver's range-check kills the card context.

**Known stubs (per `notes/AUDIT.md`).** The original C `CoreAudio` `module_init` was entirely empty. The Lisp rewrite is functionally complete for what the current drivers expose; there is no streaming API, no PCM mixing, and no latency/format negotiation — coreaudio routes single-card requests and does not mix across cards.
