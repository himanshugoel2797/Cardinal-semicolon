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

Done: multi-controller bring-up, the full endpoint model (speakers/headphones/
line-out + mics/line-in), output playback on the primary output endpoint, and
per-endpoint volume/mute (codec graph walk + volume math unit-tested with a mock
codec — SysTest `check_hdaudio` / `check_hdaudio_volume`).
TODO (follow-up PRs): capture (driving the input stream descriptors so mics/
line-in record), and codec/jack hotplug.
