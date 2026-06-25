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

Done: multi-controller bring-up, the full endpoint model (speakers/headphones/
line-out + mics/line-in), and output playback on the primary output endpoint (the
codec graph walk is unit-tested with a mock codec — SysTest `check_hdaudio`).
TODO (follow-up PRs): per-endpoint volume control, capture (driving the input
stream descriptors so mics/line-in record), and codec/jack hotplug.
