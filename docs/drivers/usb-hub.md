# usb-hub

> USB hub class driver (bInterfaceClass 0x09): probes hub descriptors, powers downstream ports, and drives connect/disconnect enumeration via coreusb.

| | |
|---|---|
| **Source** | `lisp/drivers/usb-hub.clp` |
| **Kind** | driver |
| **Bound by** | `lisp/init.clp` — always (USB hub is always needed if any USB hub device may appear) |
| **Registers with** | [coreusb](../servers/coreusb.md) via `register-class USB-CLASS-HUB` |
| **Capabilities** | none — no `sys-*` imports; all hardware access is via control-transfer messages to the host-controller context |

## Overview

`usb-hub` is the USB hub class driver (class byte `0x09`). It registers itself with the [coreusb](../servers/coreusb.md) service as the handler for any enumerated device whose interface class is `USB-CLASS-HUB`. When a hub is probed, the driver reads its class-specific hub descriptor to learn the downstream port count, tells the host controller to treat the device as a hub (`usb-mark-hub`), powers all ports, and then spawns a dedicated poll context that monitors each port's status at a 200 ms cadence. On downstream connect events the poll context resets the port and requests coreusb to enumerate the newly-attached device (which then dispatches to its own class driver). On disconnect it requests coreusb to tear the device down.

Neither the class-driver context nor the poll contexts hold any hardware capability. Every control transfer is a message to the host-controller context that owns the bus; downstream enumeration and disconnection are fire-and-forget messages to the coreusb service handle. The driver imports `coreusb` (for the proto/enum helpers) and `driver-util` (whose `serve` builds the class-driver message loop).

## Initialization

`lisp/init.clp` calls `usb-hub-init` once with the coreusb service handle:

```scheme
(usb-hub-init usb)   ; usb — the coreusb context handle
                     ; → the class-driver context handle (a serve loop)
```

`usb-hub-init` spawns a `serve` loop that owns the live-device table (a list of `(address . poll-ctx)` pairs). It then registers itself:

```scheme
(send usb (list 'register-class USB-CLASS-HUB ctx))
```

After this call, coreusb will forward `probe` and `remove` messages to this context whenever a hub-class device is attached or removed anywhere on the bus.

## Message protocol

The class-driver context is a `serve` loop. coreusb sends two messages to it.

### `probe`

```scheme
('probe dev)
```

- **Sender:** coreusb enumerator (after SET_CONFIGURATION succeeds).
- **Payload:** `dev` — the enumerated-device record (`make-usb-dev`: hci handle, bus address, speed, MPS, config bytes, config length).
- **Reply:** none (fire-and-forget from coreusb's perspective).

**What it does:**

1. Issues a class-device `GET_DESCRIPTOR(HUB, type=0x29)` requesting 8 bytes via `usb-control-in`. Uses `REQ-CLASS-DEVICE` (`0x20`).
2. If the descriptor is missing or shorter than 3 bytes, logs `[usb-hub] hub descriptor read failed; not claiming` and returns without claiming the device.
3. Reads `bNbrPorts` from descriptor byte 2; caps at `HUB-MAX-PORTS` (15).
4. Calls `usb-mark-hub dev nports` → sends `(mark-hub addr nports reply)` to the HCI so it routes downstream transactions correctly (a no-op reply on UHCI; xHCI acts on it).
5. Powers every port 1..nports via `hub-set-feature dev port PORT-POWER` (`PORT-POWER = 8`).
6. Sleeps 100 ms (port power-good settle period).
7. Spawns a restricted poll context (`spawn-restricted '() (lambda () (hub-poll usb dev nports))`).
8. Inserts `(address . poll-ctx)` into the device table.

### `remove`

```scheme
('remove addr)
```

- **Sender:** coreusb on root-port or downstream disconnect, after looking up the class from its device record.
- **Payload:** `addr` — the bus address of the hub being removed.
- **Reply:** none.

**What it does:** Finds the entry in the device table matching `addr`, sends `'stop` to its poll context (which causes the poll to exit on its next mailbox check), logs `[usb-hub] hub removed`, and drops the entry from the table.

## Poll context

Each claimed hub has one long-running poll context spawned by `hub-on-probe`. It has no capability imports and communicates only via the message API:

```scheme
(hub-poll usb dev nports)
```

- `usb` — the coreusb service handle.
- `dev` — the hub's device record (for control transfers to the hub itself).
- `nports` — downstream port count (already capped at 15).

The context maintains a `(make-bytes (+ nports 1))` vector `enumed` (1-based; byte at index `port` is 1 if that port has been enumerated, 0 otherwise). On each 200 ms iteration it scans ports 1..nports:

### Connect path (new device)

Triggered when `hub-port-status` returns a value with `PS-CONNECTION` (bit 0) set and the port is not yet tracked as enumerated.

1. Sets `enumed[port] = 1`.
2. Clears `C-PORT-CONNECTION` (feature 16) via `hub-clear-feature`.
3. Asserts port reset: `hub-set-feature dev port PORT-RESET` (feature 4).
4. Sleeps 50 ms.
5. Clears `C-PORT-RESET` (feature 20) via `hub-clear-feature`.
6. Sleeps 20 ms.
7. Re-reads port status (`hub-port-status`). If `PS-ENABLE` (bit 1) is set (the port came up), determines speed from bit 9 (`PS-LOW-SPEED = 0x200`): `USB-SPEED-LOW` if set, `USB-SPEED-FULL` otherwise.
8. Calls `usb-enumerate-downstream usb dev port speed` — fires `(enumerate-downstream hci parent-addr port speed)` to coreusb, which spawns an enumerator context for the downstream device.

### Disconnect path (device removed)

Triggered when `PS-CONNECTION` is clear and `enumed[port]` was 1.

1. Sets `enumed[port] = 0`.
2. Calls `usb-disconnect-downstream usb dev port` — fires `(disconnect-downstream hci parent-addr port)` to coreusb, which tears down the device record and notifies the class driver.

### Stop condition

At the top of every iteration the loop calls `(%mailbox-empty?)`. If any message is pending (the `'stop` sent by `hub-on-remove`, or any stray message), the loop exits with `'stopped`. The check is non-blocking; a message arriving mid-iteration is caught at the next iteration boundary.

## Exported functions

### `(usb-hub-init usb)`

The sole export. Registers the hub class driver with coreusb and returns the class-driver context handle. Must be called after `start-usb-service` has returned the coreusb handle.

## Hub wire helpers (internal)

These are not exported but are the load-bearing primitives inside the driver:

### `(hub-port-status dev port)`

Issues a class-other `GET_STATUS` to port `port` (4-byte request, `REQ-CLASS-OTHER | USB-REQ-DIR-IN`). Returns the lower 16 bits of the port-status word, or `-1` on failure or short response.

### `(hub-set-feature dev port feature)`

Issues a class-other `SET_FEATURE` to port `port`. Used for `PORT-POWER` (8) and `PORT-RESET` (4).

### `(hub-clear-feature dev port feature)`

Issues a class-other `CLEAR_FEATURE` to port `port`. Used for `C-PORT-CONNECTION` (16) and `C-PORT-RESET` (20).

## Constants

| Name | Value | Meaning |
|---|---|---|
| `HUB-DESC-TYPE` | `0x29` | Hub class descriptor type byte |
| `HUB-MAX-PORTS` | 15 | Maximum downstream ports tracked |
| `PORT-RESET` | 4 | Hub feature selector: port reset |
| `PORT-POWER` | 8 | Hub feature selector: port power |
| `C-PORT-CONNECTION` | 16 | Hub feature selector: connection-change clear |
| `C-PORT-RESET` | 20 | Hub feature selector: reset-change clear |
| `PS-CONNECTION` | 1 | wPortStatus bit: device connected |
| `PS-ENABLE` | 2 | wPortStatus bit: port enabled |
| `PS-LOW-SPEED` | `0x200` | wPortStatus bit: low-speed device |
| `REQ-CLASS-OTHER` | `USB-REQ-TYPE-CLASS \| USB-REQ-RECIP-OTHER` | bmRequestType for port-targeted requests |
| `REQ-CLASS-DEVICE` | `USB-REQ-TYPE-CLASS` | bmRequestType for hub-device requests |

## Notes / gotchas

**Speed detection is coarse.** The poll context only tests `PS-LOW-SPEED` (bit 9 of wPortStatus). A device that is neither low-speed nor annotated as low-speed is reported as `USB-SPEED-FULL` to coreusb. USB 2.0 high-speed detection (which requires a different chirp-handshake and a separate status bit) is not implemented; a high-speed device behind a hub is enumerated at full-speed.

**Port cap at 15.** `bNbrPorts` is read from the hub descriptor but silently clamped to `HUB-MAX-PORTS = 15`. Physical hubs with more ports (unusual but legal) will have their extra ports ignored.

**Stop is not immediate.** `hub-on-remove` sends `'stop` to the poll context, but the poll only checks its mailbox at the top of each 200 ms cycle. There is up to a 200 ms lag between hub removal and the poll context exiting. During this window further port-status reads will fail (the hub is gone), which is handled gracefully via the `(>= st 0)` guard.

**No re-enumeration on power-cycle.** If a port's `PS-ENABLE` is not set after the reset+settle sequence, the downstream device is silently not enumerated for this connect event. The next 200 ms poll will see `PS-CONNECTION` still set and `enumed[port]` already 1, so no re-attempt is made. A physical device that requires a long reset de-bounce may be missed.

**Hub descriptor is read at default (8-byte) size.** Only bytes 0–2 of the hub descriptor are consumed (`bDescLength`, `bDescriptorType`, `bNbrPorts`). The power-on-to-power-good delay field (`bPwrOn2PwrGood`, byte 5) is not read; a fixed 100 ms settle is used instead, which is safe for most hubs but may be short for hubs specifying a longer delay.

**Poll context holds no capabilities.** `spawn-restricted '()` gives the poll context an empty capability set. All transfers go through messages to the HCI context; the hub driver never imports any `sys-*` module.

**`await-complete` drops non-completion messages.** The hub control-transfer helpers (`hub-port-status`, `hub-set-feature`, `hub-clear-feature`) go through `usb-control-in` / `usb-control-out`, which internally call `await-complete`. `await-complete` silently discards any message that is not a `'complete` reply. In the hub poll context this is safe because the only expected messages are transfer completions and the eventual `'stop`; however, if `'stop` arrives while `await-complete` is looping, it is dropped and the stop will be missed until the next mailbox check at the top of the next poll iteration.
