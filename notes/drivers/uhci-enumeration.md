# UHCI control transfers + USB enumeration — implementation plan

This is the implementation-ready design for the next USB step (see
`notes/servers/CoreUsb-roadmap.md` for the broader roadmap). It is written as a
note rather than code because the **CoreUsb transfer API is a consequential
cross-module interface** that should be reviewed before class drivers build on
it. The UHCI-internal mechanics below, however, are well-defined by the spec and
can be implemented directly.

## Current state (this branch fixed two prerequisites)

- `uhci_reset`/`uhci_enableport` previously called the broken `task_sleep`
  (which does not deschedule — see `notes/AUDIT.md`), so the reset/port-reset
  delays never actually happened. Now they use `uhci_delay_ns` (a wall-clock
  busy-wait off `timer_timestamp_ns`), so reset timing is real.
- `transfer_descriptor_t.status.act_len` was 10 bits; UHCI ActLen is **11 bits**,
  which had misaligned every field above it in the dword. Fixed.

Still missing: any transfer mechanism (the frame list is all-invalid) and any
enumeration. The TD struct exists; there is no Queue Head struct.

## UHCI control-transfer mechanics (USB 1.1 / UHCI 1.1 spec)

All descriptor memory (QH, TDs, data buffers, the 8-byte SETUP packet) must be
**32-bit physical, 16-byte aligned** — use
`pagealloc_alloc(..., physmem_alloc_flags_32bit | _data | _zero, ...)` then
`vmem_phystovirt(..., vmem_flags_uncached | _kernel | _rw)`. Link/element
pointers in QH/TD are **physical** addresses (`phys >> 4` into the 31:4 field).

### Queue Head (add to uhci.h)
```
typedef struct qh {                 // 8 bytes used, pad/align to 16
    union { struct { uint32_t terminate:1, is_qh:1, z:2, qhlp_31_4:28; }; uint32_t hlp; } head;  // -> next QH (or T)
    union { struct { uint32_t terminate:1, is_qh:1, z:2, qelp_31_4:28; }; uint32_t elp; } elem;  // -> first TD (or T)
} PACKED uhci_qh_t;
```
Schedule layout: point one (or several) frame-list entries at a control QH
(`is_qh=1`). The QH's element pointer points at the first TD of the current
transfer; `terminate=1` when idle.

### TD token PIDs / fields
- PID: `SETUP=0x2D`, `IN=0x69`, `OUT=0xE1`.
- `token.maxlen` = (data length − 1) & 0x7FF; **0x7FF means zero-length**
  (e.g. the STATUS stage and the SETUP-of-an-IN's data toggle handling).
- `token.data_toggle`: SETUP=0; first DATA stage TD=1, then alternate; STATUS=1.
- `token.device`/`token.endpoint`: target address/endpoint (0/0 before
  SET_ADDRESS).
- `status.status` = 0x80 (Active). `status.err_count` (C_ERR) = 3.
  `status.ls` = 1 for a **low-speed** device (PORTSC bit 8). `status.ioc` only if
  you want an interrupt (we poll).
- `link`: chain TDs depth-first (`link.depth_first=1`), last TD `link.terminate=1`.

### A control IN transfer (e.g. GET_DESCRIPTOR)
1. SETUP TD: PID=SETUP, toggle=0, maxlen=7 (8 bytes), buffer=phys(setup_pkt).
2. DATA TDs: PID=IN, toggle alternating from 1, each maxlen=mps−1 (mps=8 for ep0
   until the real max packet size is known), buffer=phys(data + offset). Stop
   when the requested length is covered (or a short packet arrives).
3. STATUS TD: PID=OUT, toggle=1, maxlen=0x7FF (zero length), buffer=0.
4. Point the control QH's element pointer at the SETUP TD; ensure a frame-list
   entry points at the QH; the controller is already running (`USBCMD.RS`).
5. **Poll** the last TD's `status.status` Active bit clear (with a timeout via
   `uhci_delay_ns`/`timer_timestamp_ns`). Check the error bits (Stalled/CRC/
   Babble) in each TD; success = all TDs retired, not Active, no error.
6. Read `act_len` from the data TDs for the bytes actually transferred.
(For a control OUT, swap IN/OUT in stages 2 and 3.)

## Enumeration sequence (per newly-connected, reset, enabled port)
1. `GET_DESCRIPTOR(Device)` first **8 bytes** at address 0 (ep0 mps unknown →
   use 8). Read `bMaxPacketSize0` (offset 7).
2. `SET_ADDRESS(n)` (control OUT, no data) — assign a unique address; then a
   short delay (≥2ms) before using the new address.
3. `GET_DESCRIPTOR(Device)` full 18 bytes at address n, real mps. Read
   idVendor/idProduct/bDeviceClass.
4. `GET_DESCRIPTOR(Configuration)` (9 bytes, then full `wTotalLength`) — parse
   interface/endpoint descriptors.
5. `SET_CONFIGURATION(bConfigurationValue)`.
6. Hand off to a class driver keyed on interface class (HID=3, Hub=9,
   MassStorage=8). **This requires** adding a class/type to `usb_device_t` (it
   has none today — see `notes/AUDIT.md` CoreUsb item) and a class→driver match.

Standard SETUP packets (8 bytes, little-endian): GET_DESCRIPTOR =
`{0x80, 0x06, descIndex, descType, 0, 0, lenLo, lenHi}`; SET_ADDRESS =
`{0x00, 0x05, addrLo, 0, 0,0, 0,0}`; SET_CONFIGURATION =
`{0x00, 0x09, cfgVal, 0, 0,0, 0,0}`.

## Proposed CoreUsb transfer API (the part to review before building on it)
Keep it minimal and HC-agnostic; the HC driver implements it, CoreUsb/class
drivers call it:
```
// Synchronous control transfer on endpoint 0. setup is the 8-byte packet;
// data/len is the (optional) data stage; returns bytes transferred or <0.
int usb_control_transfer(void *hc_handle, int dev_addr, int low_speed,
                         const void *setup, void *data, int len);
// Open/poll an interrupt IN endpoint (for HID); shape TBD.
```
Open questions to settle in review: sync vs async (a transfer-complete
callback/event vs blocking), where enumeration lives (CoreUsb vs each HC
driver — recommend CoreUsb so it is shared), how addresses are allocated, and how
this maps onto `SysObj`. Until then, a control transfer can be prototyped
**inside the UHCI driver** (a static `uhci_control_transfer` + a temp
`CALL:`-triggered "read device descriptor and print it" self-test, mirroring the
network `network_debug_selftest`) to prove the mechanics without committing the
cross-module API.

## Testing under QEMU (TCG only — KVM hangs the boot, see AUDIT)
Attach a UHCI controller + a low-speed device:
```
EXTRA="-device piix3-usb-uhci,id=uhci -device usb-kbd,bus=uhci.0"
ACCEL=tcg TIMEOUT=150 ./scripts/run-qemu.sh
```
`usb-kbd`/`usb-mouse` are low-speed (set `status.ls`). Expect the existing
`[UHCI] Device connected` print on the polling task; the descriptor read should
then return `idVendor=0x0627` (QEMU) for usb-kbd. Watch for the broken-`task_sleep`
class of timing bugs and data-toggle mistakes (a wrong toggle → the device NAKs
forever → transfer times out).
```
