<!---
 Copyright (c) 2018 Himanshu Goel
 
 This software is released under the MIT License.
 https://opensource.org/licenses/MIT
-->

The audio system provides a means for applications to stream audio data straight to the driver, it works with the display subsystem to support HDMI audio output. It also provides microphone input. The USB system connects into it to provide usb audio input/output.

## Current implementation (Lisp)

`coreaudio` (`lisp/servers/coreaudio.clp`) is the front door: a card driver
registers its **driver context** plus a list of **endpoint descriptors**, and
clients route messages (`tone`/`play`/`cards`/`endpoints`) to a named card. It
never touches hardware — it just forwards to the owning driver context, the same
least-privilege posture as the other `Core*` services.

The HD Audio driver (`lisp/drivers/hdaudio.clp`) brings up **every** HDA
controller on the bus (init enumerates them with `pci-find-class-all #x04 #x03`
and registers each as `hda0`, `hda1`, …). For each codec it walks the full audio
function group and builds an **endpoint** per usable pin: it classifies the pin by
its *configuration default* device (speaker / headphone / line-out / mic /
line-in / …) and its in/out pin-caps, resolves it to a converter (an output pin to
its DAC through the connection list; an input pin to the ADC that reaches it), and
records the jack's presence. An endpoint is a mutable record so later work can
update its volume / presence in place.

Per-endpoint **volume** is a 0..100 percentage mapped onto the target amp's step
range (`SET_AMP_GAIN_MUTE`; 0 mutes). The target is the pin's own amp when it has
one — so a speaker and a headphone jack sharing one DAC get *independent* volume —
otherwise the converter's amp. Messages: `(set-volume name ep-id vol)` /
`(get-volume name ep-id reply)` / `(mute name ep-id on?)`; the serial REPL exposes
`(set-vol ep-id vol)`.

**Capture** drives the first input stream descriptor: `configure-input!` powers the
ADC + input pin, enables the pin's input (`PIN_CONTROL` IN_EN), routes the ADC to
the pin, and sets the capture format + stream tag; the controller then DMAs into a
cyclic ring. Messages: `(capture-start name ep-id)` / `(capture-read name reply)`
(a bytes copy of the ring) / `(capture-pos name reply)` (DMA position) /
`(capture-stop name)`. `cardinal.mictest` is a boot self-check.

**Hotplug**: each endpoint carries its own codec handle, so a card can span codecs
(or gain one later). `scan-all-codecs` probes every codec address and rebuilds the
endpoint set — the single reconciliation point for boot AND hotplug. A watcher
context parked on the controller MSI notices a STATESTS change (a codec
hot-added/removed) and sends the driver loop `codec-change`, which re-scans. (QEMU's
hda bus can't actually hot-add a codec, so `(rescan name)` / `cardinal.hotplug`
inject the same reconciliation to validate the path; the watcher itself is the
proven `msi-wait` pattern.)

**Jack-presence (plug-detect)**: a 1 s poller re-reads each pin's GET_PIN_SENSE
(`poll-jacks!`); when a jack's presence changes it updates the endpoint and logs
`jack <dev> inserted/removed`. This POLLS rather than enabling codec unsolicited
responses — those would interleave in the RIRB and break the solicited-response
verb poller, and QEMU emits no jack event for either mechanism, so polling is the
lower-risk path that exercises its read side live. `(poll-jacks name)` /
`cardinal.jacktest` force a poll.

Done: multi-controller bring-up, the full endpoint model (speakers/headphones/
line-out + mics/line-in), output playback, per-endpoint volume/mute, capture
(mic/line-in), codec hotplug, and jack-presence detection — the HD Audio stack is
complete. Unit-tested with a mock codec — SysTest `check_hdaudio` /
`check_hdaudio_volume` / `check_hdaudio_capture` / `check_hdaudio_multicodec` /
`check_hdaudio_jack` (41/0); capture DMA validated live (`cardinal.mictest`), the
hotplug reconciliation live (`cardinal.hotplug`), and the jack-poll read-path live
(`cardinal.jacktest`).
