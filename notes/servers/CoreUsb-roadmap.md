# USB stack — status and roadmap

Goal (per project direction): a working USB device stack — **hub, HID keyboard,
HID mouse, mass storage**. This note captures the current state and a
review-friendly path, so the work can proceed incrementally rather than as one
large unreviewable drop.

## Current state (2026-06)

- **Host controllers**: `drivers/uhci` and `drivers/ehci` exist and are bound via
  `devices.txt` (class 0C/03, progif 00=UHCI, 20=EHCI). UHCI `module_init` sets up
  the frame list, calls `usb_register_hostcontroller`, resets the HC, and enables
  ports. EHCI is more skeletal (registers, resets; port enable commented out).
  Neither HC supports interrupts — both intend a **polling task** (TODO in both).
- **CoreUsb** (`servers/CoreUsb`): tracks host controllers and devices
  (`usb_register_hostcontroller` / `usb_register_device` / `usb_remove_device`),
  spawning a per-device kernel task. Mid-flight. (A fixed OOB bug — `def->type`
  used uninitialised as a `devIDs[]` index — was patched on branch
  `claude/kernel-robustness`; see `notes/AUDIT.md`.)
- **CoreInput** (`servers/CoreInput`): has an `input_device_register` API that a
  HID driver would target. PS/2 (`drivers/ps2`) is the only current input source.
- **Missing entirely**: USB **enumeration** (port reset → address assignment →
  GET_DESCRIPTOR → set configuration), the **transfer abstraction** (control /
  interrupt / bulk pipes) exposed by CoreUsb to class drivers, and **all class
  drivers** (hub, HID, mass storage).

## The gap that gates everything: enumeration + a transfer API

Class drivers (HID, hub, mass storage) cannot be written until CoreUsb exposes a
**device-/endpoint-level transfer API** (submit a control transfer, open an
interrupt IN pipe, do a bulk transfer) that the HC drivers implement. Today
CoreUsb only does registration bookkeeping; the HC drivers own the frame
list/queue heads but expose no generic submit path. This API is the key
**consequential interface** — it should be designed and reviewed before the class
drivers pile on top of it.

There is also a known structural gap: `usb_device_t` (`servers/inc/CoreUsb/usb.h`)
carries no device **class/type** field, so enumerated devices can't yet be routed
to the right class driver. Adding that (and a class→driver match akin to
`devices.txt`) is part of this work.

## Recommended incremental path (each a reviewable step)

1. **Polling task + transfer primitives on one HC (UHCI first).** Give UHCI a
   polling task and implement control + interrupt transfers behind a small CoreUsb
   API (`usb_control_transfer`, `usb_interrupt_open`/`poll`). Validate with raw
   descriptor reads.
2. **Enumeration in CoreUsb.** Port reset, address assignment, GET_DESCRIPTOR
   (device/config), SET_CONFIGURATION; populate a real `usb_device_t` incl. a new
   class/type field; match to a class driver.
3. **Hub driver.** Required for >1 device and for most QEMU topologies; depends on
   (1)+(2).
4. **HID boot keyboard, then mouse.** Boot protocol is simple (8-byte reports);
   feed into CoreInput via `input_device_register`. Good first end-to-end win.
5. **Mass storage (BBB / SCSI over bulk).** Depends on bulk transfers; ties into
   `CoreStorage` (see `notes/servers/CoreStorage/filesystem-direction.md`).

## On temporary/test interfaces

It is fine to stand up throwaway interfaces to explore and test these (and to
exercise the network stack bidirectionally) — but keep them **clearly marked and
isolated** so the real interfaces can be designed afterward:

- Put exploratory glue behind an obvious name/comment (e.g. a `// TEMP:`/
  `_debug_` prefix) and in its own file where practical, so it's trivial to grep
  and delete.
- Don't let a temporary API leak into the public headers under `*/inc/` that
  other modules build against — that's what makes a placeholder hard to redesign.
- For network bring-up specifically, the existing `network_tx_packet` (L2
  broadcast) plus `ipv4_tx`/`ethernet_tx` are enough to send test traffic
  outbound; a temporary "inject a UDP packet" debug entry point is a reasonable
  way to test the tx path without committing to the socket API.
