# USB stack — implemented (this branch)

A working basic USB stack, all validated end-to-end under QEMU/KVM with real
devices (the apic-timer-kvm fix is merged in, so KVM boots).

## What works

| Piece | Where | Validation |
|-------|-------|------------|
| Host controller (UHCI) | `drivers/uhci` | enumerates devices on a `piix3-usb-uhci` |
| Host controller (xHCI) | `drivers/xhci` | same class drivers drive a `qemu-xhci`: kbd + mass storage verified |
| Control transfers | `drivers/uhci` (`uhci_control_transfer`) | device + config descriptor reads |
| Interrupt / bulk transfers | `drivers/uhci` (`uhci_data_transfer`) | HID polling, mass-storage BBB |
| Enumeration + class dispatch | `servers/CoreUsb/src/enum.c` | addr assign, descriptors, SET_CONFIGURATION, class match |
| HID keyboard + mouse | `drivers/usb_hid` | injected keys `a/b/c` decode to usage 0x4/0x5/0x6 ('a'/'b'/'c') |
| Mass storage (BBB + SCSI) | `drivers/usb_storage` | INQUIRY="QEMU QEMU HARDDISK", READ CAPACITY, READ(10) returns the disk's LBA0 bytes |
| Hub + downstream enumeration | `drivers/usb_hub` | a keyboard behind a hub enumerates and its keys arrive |

### Reproduce
```
ACCEL=kvm  # KVM boots fast now; TCG also works
# keyboard:  -device piix3-usb-uhci,id=uhci -device usb-kbd,bus=uhci.0
# mouse:     ... -device usb-mouse,bus=uhci.0
# storage:   ... -drive id=d,file=disk.img,if=none,format=raw -device usb-storage,bus=uhci.0,drive=d
# hub+kbd:   ... -device usb-hub,bus=uhci.0,port=1 -device usb-kbd,bus=uhci.0,port=1.1
```
Inject input headlessly via the QEMU monitor (`sendkey a`) over a `-monitor unix:`
socket; watch COM1 for the decoded reports.

## Architecture

Controller-agnostic transfer layer in CoreUsb (`usb_hci_handlers_t`:
control/interrupt_in/bulk on `usb_hci_desc_t`). Enumeration lives in CoreUsb;
class drivers register a probe per `bInterfaceClass` via
`usb_register_class_driver` (the same self-registration idiom as
`network_register`/`display_register`) and are loaded by `servicescript` before
CoreDriver. The host-controller driver implements the transfer handlers and
calls `usb_port_connected` when a port comes up; the hub driver calls
`usb_dev_enumerate_downstream` for its ports.

## Deliberately simple / to be reviewed and redesigned

These are the throwaway/first-cut interfaces the project asked to keep easy to
redesign:

- **Class drivers print to the debug console** instead of delivering up the
  stack. HID should feed **CoreInput** (its `read` pull-model wants an event
  queue), and mass storage should register a **block device with CoreStorage**
  (which ties into `notes/servers/CoreStorage/filesystem-direction.md`). The
  control/interrupt/bulk transfer logic underneath is real and reusable.
- **The CoreUsb transfer API is synchronous/blocking** (one transfer at a time
  per controller under a lock) and uses **bounded busy-spin** delays/timeouts
  (because `task_sleep` doesn't deschedule and `timer_timestamp_ns` is unreliable
  here — see `notes/AUDIT.md`). An async/event-driven, non-busy-waiting API is the
  right longer-term shape.
- **`usb_device_t` still has no class/type field**; enumerated devices are
  dispatched directly to a class probe rather than registered as `usb_device_t`s.
- **Toggle/halt handling is minimal** (no CLEAR_FEATURE(HALT) recovery on STALL;
  per-endpoint toggle reset only at init). Fine for the tested paths.

## Controllers

Both **UHCI** (`drivers/uhci`) and **xHCI** (`drivers/xhci`) implement the
controller-agnostic `usb_hci_handlers_t`; the enumeration layer and all class
drivers are shared unchanged. EHCI remains a stub and could be added the same
way. xHCI notes:

- xHCI's slot/command model is adapted to CoreUsb's address-based enumeration:
  on connect it does Enable Slot + Address Device(BSR=1) so EP0 works at the
  default address, the control handler intercepts SET_ADDRESS → Address
  Device(BSR=0), and non-control endpoints are Configure-Endpoint'd lazily on
  first transfer.
- Polled (no interrupts) like UHCI; single interrupter, single event-ring
  segment; 64-byte-or-32-byte contexts read from HCCPARAMS.
- **Hub-on-xHCI is not yet supported**: a device behind a hub needs the slot
  context's route string + parent-hub-slot/port fields, which the first-cut
  Address Device path doesn't populate (it only sets the root port). Hubs work
  on UHCI. Adding route-string support to `xhci_address_device` is the fix.
