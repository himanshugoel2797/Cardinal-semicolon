# Lisp device drivers API

The device drivers under `lisp/drivers/` are Cardinal Lisp modules that run over
the driver substrate (`sys-mmio`, `sys-pci`, `sys-io`, `sys-irq`, `sys-reg`, the
shared `virtio` transport, and the generic `driver-util` helpers). Each driver is
a `(define-module …)` whose `(export …)` list is the authoritative inter-module
surface; only those symbols are documented here. Some modules are split across
private files spliced in with `(include …)` — for those the `source:` points at
the component that holds the `(define …)`.

Almost every driver exposes a single `*-init` entry point that `lisp/init.clp`
calls (gated on a `pci-find`, so a boot without the device just logs and
returns). The init contract is uniform: discover the device, map its registers,
bring it up inside a *spawned* context (so the reset/spin-up/completion waits can
yield), and register with the relevant `Core*` service. The few extra exports are
pure builders/parsers/register-offset constants kept public for the
hardware-free in-OS self-tests.

---

## `ahci-init`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/drivers/ahci/driver.clp`
- **hash:** 4282c15ca0a09c47

Entry point for the SATA AHCI block-device driver: discovers QEMU's ICH9 AHCI
controller, maps its ABAR, and spawns a context that brings up the HBA/port,
runs IDENTIFY, and registers a block device with corestorage.

**Parameters**
- `storage` — the corestorage service handle (the driver sends it
  `(register-blockdev 'ahci0 512 sectors driver-ctx)` once a disk comes up).

**Behavior.** `pci-find`s the ICH9 AHCI controller (VID `0x8086`, DID `0x2922`
— matched on VID/DID only, so it binds the ICH9 specifically). If absent it logs
`[ahci] no device present` and returns `#f`. Otherwise it maps the ECAM, enables
memory + bus-master, locates the ABAR (BAR5, self-assigning BARs and re-reading
BAR5 if firmware left it unconfigured), maps the HBA register window, and
**spawns** a restricted bring-up context (`ahci-bringup`). The bring-up does
BIOS/OS handoff, HBA reset, scans implemented ports for the first with a live
link + ready device, IDENTIFYs it, sets up MSI, arms the port/global interrupt
*last*, runs a read/write-back smoke, and registers the block device. The driver
context then answers corestorage `(read lba count reply)` /
`(write lba count data reply)` requests.

**Returns:** `'ahci-spawned` once the bring-up context is launched, or `#f` if no
controller / no ABAR was found. The bring-up itself is fire-and-forget (its
result goes to the log, not the caller), because the caller — boot `system-init`
— is not running under the scheduler and cannot block.

---

## `fis-build!`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/drivers/ahci/ata.clp`
- **hash:** ae07967c4180a8ab

Builds an ATA Host-to-Device register FIS (the 20-byte on-wire command FIS) into
a byte buffer at offset 0.

**Parameters**
- `fis` — the destination byte buffer (the command table; the first 20 bytes are
  zeroed then filled).
- `command` — ATA command opcode (e.g. `ATA-IDENTIFY`, `ATA-READ-EXT`,
  `ATA-WRITE-EXT`).
- `lba` — 48-bit logical block address (laid out byte-by-byte across LBA 7:0…47:40).
- `count` — sector count (low/high bytes at FIS offsets 12/13).
- `device` — the device register byte (`0x40` = LBA mode for the data commands;
  `0` for IDENTIFY).

Sets FIS type `0x27` (Register H2D) and the C (command) bit, then writes each
positional byte so the result is endianness-independent. **Returns** the `fis`
buffer.

---

## `prdt-set!`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/drivers/ahci/ata.clp`
- **hash:** d07cce20d107259e

Programs PRDT entry 0 (the single physical-region descriptor) in the AHCI command
table.

**Parameters**
- `ctbl` — the command-table byte buffer (the PRDT begins at `PRDT-OFF` = `0x80`).
- `data-phys` — physical base address of the data buffer (split into DBA/DBAU).
- `len` — data byte count; the descriptor stores `len-1` in DBC.
- `s64a?` — whether the HBA supports 64-bit addressing (controls whether the high
  DWORD of the address is written).

Always sets bit31 (interrupt-on-completion) in DBC. **Returns** the `ctbl` buffer.

---

## `cmdhdr-set!`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/drivers/ahci/port.clp`
- **hash:** 3272275d291034f7

Fills command-header slot 0 (32 bytes at command-list offset 0) pointing at a
command table.

**Parameters**
- `cmdlist` — the command-list byte buffer.
- `ctbl-phys` — physical address of the command table (CTBA/CTBAU).
- `cfl-dwords` — command-FIS length in DWORDs (a 20-byte H2D FIS → 5).
- `write?` — `#t` sets the W bit (host→device transfer).
- `prdtl` — number of PRDT entries (1 here).

Writes DWORD0 (CFL | W | PRDTL), clears PRDBC, and writes the command-table base
address. **Returns** the result of the last `bytes-u32-set!` (used for its
effect, not its value).

---

## `id-sector-count`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/drivers/ahci/ata.clp`
- **hash:** 66e4d41228318e37

Extracts the total addressable sector count from an IDENTIFY data buffer.

**Parameters**
- `buf` — the 512-byte IDENTIFY response (256 little-endian 16-bit words).

If word 83 bit10 (LBA48 supported) is set, returns the 48-bit count from words
100–103; otherwise the LBA28 count from words 60–61. **Returns** the sector count
as an integer.

---

## `id-model`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/drivers/ahci/ata.clp`
- **hash:** bf1bb3d24167f7c0

Extracts the model string (IDENTIFY words 27–46) from an IDENTIFY data buffer.

**Parameters**
- `buf` — the 512-byte IDENTIFY response.

Swaps each word's byte order back (the model is stored high-byte-first) and trims
trailing spaces/NULs. **Returns** the model as a Lisp string.

---

## `id-word`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/drivers/ahci/ata.clp`
- **hash:** e5db7801df759f8e

Reads IDENTIFY word `w` (the little-endian 16-bit value at byte `2*w`) from an
IDENTIFY buffer.

**Parameters**
- `buf` — the 512-byte IDENTIFY response.
- `w` — the word index (0–255).

**Returns** the 16-bit word value.

---

## `ATA-IDENTIFY`

- **kind:** lisp-const
- **lang:** lisp
- **source:** `lisp/drivers/ahci/ata.clp`
- **hash:** 3e0d38014450a035

The ATA IDENTIFY DEVICE opcode, `0xEC`.

Used by `identify` to fetch the 512-byte device parameter block (sector count,
model). One of the three ATA opcodes the driver issues.

---

## `ATA-READ-EXT`

- **kind:** lisp-const
- **lang:** lisp
- **source:** `lisp/drivers/ahci/ata.clp`
- **hash:** 977ec9f2f45f5022

The ATA READ DMA EXT (LBA48) opcode, `0x25`.

Used by `read-sectors` to read `count` sectors from `lba` via DMA into the data
buffer.

---

## `ATA-WRITE-EXT`

- **kind:** lisp-const
- **lang:** lisp
- **source:** `lisp/drivers/ahci/ata.clp`
- **hash:** 29a171ef0db59b64

The ATA WRITE DMA EXT (LBA48) opcode, `0x35`.

Used by `write-sectors` to write `count` sectors from the data buffer to `lba`;
the command header marks the transfer host→device.

---

## `PRDT-OFF`

- **kind:** lisp-const
- **lang:** lisp
- **source:** `lisp/drivers/ahci/port.clp`
- **hash:** 6418a59349ef0282

Byte offset of the PRDT within the AHCI command table, `0x80`.

The command table holds the H2D command FIS at offset 0 (`FIS-OFF`) and the PRDT
from `PRDT-OFF`; `prdt-set!` writes its single entry here.

---

## `FIS-OFF`

- **kind:** lisp-const
- **lang:** lisp
- **source:** `lisp/drivers/ahci/port.clp`
- **hash:** 31ba3b48ab339cf3

Byte offset of the command FIS within the AHCI command table, `0x00`.

The H2D register FIS built by `fis-build!` occupies offset 0 of the command
table; the PRDT follows at `PRDT-OFF`.

---

## `lfb-init`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/drivers/lfb.clp`
- **hash:** 9890aa10d87ad108

Entry point for the linear-framebuffer display driver: reads the boot
framebuffer's geometry from SysReg, maps it, and spawns a context that paints a
test pattern and registers a "Linear Framebuffer" display with coredisplay.

**Parameters**
- `display-svc` — the coredisplay service handle.

**Behavior.** Reads `PHYS_ADDR`/`PITCH`/`WIDTH`/`HEIGHT` (and the colour-channel
bit offsets, defaulting to the 16/8/0 X8R8G8B8 layout) from
`HW/BOOTINFO/FRAMEBUFFER`. If any geometry key is missing or zero it logs
`[lfb] no boot framebuffer; not registering` and returns `#f` (no fallback
display). Otherwise it maps the framebuffer MMIO (`pitch*height` bytes) and
**spawns** a restricted driver context that paints a computable RGB gradient
(the live proof the mapping reaches scanned-out pixels — done on the spawned
context's GC'd heap, not the root init heap), registers with coredisplay via
`lfb-register-msg`, and serves `get-framebuffer` / `get-displayinfo` /
`get-status` / `flush` (a no-op — the framebuffer is directly scanned out) /
`fill` requests. There is no device handshake, DMA, or IRQ — only `sys-reg` and
`sys-mmio`, the narrowest-capability display driver in the tree.

**Returns:** the spawned driver-context handle, or `#f` when there is no boot
framebuffer.

---

## `pack-rgb`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/drivers/lfb.clp`
- **hash:** d7b66b854ada7446

Packs 8-bit R/G/B channels into a framebuffer pixel word given the per-channel
bit offsets.

**Parameters**
- `r`, `g`, `b` — the 8-bit colour channels.
- `r-off`, `g-off`, `b-off` — the bit offsets of each channel in the pixel word
  (16/8/0 for the usual X8R8G8B8 layout).

**Returns** `(r<<r-off) | (g<<g-off) | (b<<b-off)`, i.e. `0x00RRGGBB` for the
conventional layout. Pure and exported for the unit/screenshot self-test (it lets
the test compute the expected colour of a sampled pixel).

---

## `lfb-register-msg`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/drivers/lfb.clp`
- **hash:** c2a50b1c032c0eae

Sends the coredisplay registration message for a brought-up framebuffer.

**Parameters**
- `display-svc` — the coredisplay service handle.
- `width`, `height`, `pitch` — the framebuffer geometry.
- `ctx` — the driver loop's own handle (so consumers can send it
  `get-framebuffer`/`get-displayinfo`/`get-status`/`flush`/`fill`).

Sends `(register "Linear Framebuffer" 'unknown ctx (width height pitch 32))`;
coredisplay stores `(cdr m)` verbatim. Factored out so the self-test can drive
registration without hardware. **Returns** the `send` result.

---

## `ps2-init`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/drivers/ps2.clp`
- **hash:** 45c7b825f845b6c0

Brings up the i8042 PS/2 controller and the keyboard (controller self-test,
config-byte setup, enable port 1 + scanning), leaving the keyboard IRQ enabled.

Takes **no arguments**. Disables both ports, quiets the port IRQs during setup,
runs the controller self-test (`0xAA`), and on success (`0x55`) re-writes the
config, enables port 1, runs the keyboard init (enable scanning `0xF4`, consume
the ACK), then sets the keyboard-IRQ bit (config bit 0). Translation is left as
the firmware set it (the pump is scancode-set-agnostic). All waits are bounded so
absent/wedged hardware cannot hang boot.

**Returns:** `#t` on success; `#f` (after logging
`[ps2] controller self-test failed; no PS/2 input`) if the controller self-test
fails (no PS/2 hardware). Keyboard-only — port 2 (mouse) is left disabled.

---

## `ps2-keyboard-driver`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/drivers/ps2.clp`
- **hash:** 7f20e02d2b0d4fc7

The long-lived keyboard driver context: claims IRQ 1, registers with coreinput,
self-tests the interrupt path, then loops draining scancodes on each keyboard IRQ.

**Parameters**
- `coreinput` — the coreinput service handle (receives
  `(event (key <scancode> <pressed?>))` for each key, and the initial
  `(register 'ps2-keyboard)`).

**Behavior.** Spawned as a restricted context after `ps2-init`. Registers with
coreinput, `irq-register`s IRQ 1 (returns `#f` after logging if that fails), runs
`ps2-irq-selftest` (sends the keyboard a `0xEE` echo and asserts the reply
arrives via the real IRQ — QEMU doesn't deliver injected keystrokes, so this is
the headless smoke test), then pumps forever: drain the i8042 output buffer
(decoding `0xF0` break-prefix releases, ignoring `0xFA` ACKs, discarding mouse
bytes) and `irq-wait` for the next IRQ. The `seen` count is captured *before*
draining so a key landing mid-drain is never missed. Does not return under normal
operation.

---

## `rtl8139-init`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/drivers/rtl8139.clp`
- **hash:** 9e72da6e80e2513f

Entry point for the Realtek RTL8139 NIC driver: discovers and resets the device,
programs the RX ring + TX slots, spawns polling RX/TX contexts, and registers
with the corenetwork service.

**Parameters**
- `net` — the corenetwork service handle (the driver sends it
  `(register-nic mac tx-ctx)` and `(rx frame len)` for each received frame).

**Behavior.** Runs *inside* a spawned context (so `wait-until`/`sleep` yield).
`pci-find`s the RTL8139 (VID `0x10EC`, DID `0x8139`); absent → logs
`[rtl8139] no device present`, returns `#f`. Maps the MMIO registers (BAR1,
self-assigning if firmware left it unconfigured), powers on, software-resets
(bounded wait for RST to self-clear), reads the MAC, allocates the 64K RX ring +
four 2KB TX slots as **<4GB** DMA buffers (this is a legacy 32-bit-DMA device),
programs the ring/slot physical addresses, configures RX (accept
phys/multicast/broadcast + WRAP + 64K buffer) and TX, then enables RX+TX last
(IMR stays 0 — the device has no MSI, so RX is **polled**). It spawns a TX context
(answers `(tx frame len)`) and an RX poll context (drains the ring every 1ms,
snapshots each frame out of the recycled ring, forwards it to `net`), then
registers the NIC.

**Returns:** `'ok` on success; `#f` (after a logged reason) on any
discovery/reset/DMA-alloc failure.

---

## `rx-parse-one`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/drivers/rtl8139.clp`
- **hash:** 216f15dd80faccd8

Pure parser for one RTL8139 receive-ring header.

**Parameters**
- `rxbuf` — the RX ring byte buffer.
- `off` — byte offset of the 4-byte rx-header (status u16, length u16) in the ring.

Computes the frame body offset (`off+4`), the frame length minus the trailing
4-byte CRC, and the dword-aligned start of the next header wrapped modulo the 64K
ring. **Returns** `(frame-off frame-len next-off)`. Exported for the hardware-free
RX self-test.

---

## `tx-fill!`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/drivers/rtl8139.clp`
- **hash:** 43b701dde429cf6c

Transmits a frame via the RTL8139's 4-slot round-robin TX path.

**Parameters**
- `regs` — the mapped MMIO register buffer.
- `txbufs` — the list of four TX slot DMA buffers.
- `free-cell` — a cell holding the next free slot index (advanced modulo 4).
- `frame` — the frame bytes to send.
- `len` — the frame length.

Rejects `len > 2048` or `len <= 0` (returns `#f`). Otherwise waits (bounded,
yielding) for any prior transmit on the slot to finish, copies the frame into the
slot, writes its length into the per-slot TX-STS (which clears OWN and kicks the
DMA), and advances `free-cell`. **Returns** `'sent`. Exported (its buffers/cell
are args) so it is testable without hardware.

---

## `rx-extract!`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/drivers/rtl8139.clp`
- **hash:** 761a1ff145660abb

Drains every frame currently in the RTL8139 receive ring, invoking a handler for
each good one and advancing CAPR.

**Parameters**
- `regs` — the mapped MMIO register buffer.
- `rxbuf` — the RX ring byte buffer.
- `off-cell` — a cell holding the current read offset into the ring.
- `handler` — called `(handler frame-off frame-len)` for each ROK frame of at
  least a full ethernet header.

Loops while the BUFE bit is clear (data present): parses each header with
`rx-parse-one`, calls the handler for good frames, advances `off-cell`, and writes
CAPR back (trailing the read pointer by the device's 16-byte quirk) so the device
may reuse the space. **Returns** `'drained` when the ring is empty.

---

## `virtio-net-init`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/drivers/virtio-net.clp`
- **hash:** 64860af2fef85764

Entry point for the virtio-net NIC driver: brings the device to DRIVER_OK over
the shared virtio transport, sets up the RX/TX virtqueues with MSI-X, and
registers with the corenetwork service.

**Parameters**
- `net` — the corenetwork service handle (receives `(register-nic mac tx-ctx)` and
  `(rx frame len)` per received frame).

**Behavior.** `pci-find`s virtio-net (VID `0x1af4`, DID `0x1041`); absent → logs
and returns `#f`. Runs the common `virtio-bringup` accepting F_MAC + VERSION_1
(a rejected FEATURES_OK → logs, `#f`). Sets up RX queue 0 and TX queue 1 (which
programs each queue's MSI-X vector), points the config-event vector at entry 0,
enables the MSI-X capability (`pci-setup-msi`, after the vector registers per the
spec), sets DRIVER_OK, allocates RX/TX DMA buffers, and populates the RX ring. It
spawns a TX context (answers `(tx frame len)` — copies the frame after the
12-byte virtio-net header and posts it) and an MSI-driven RX context (drains the
used ring, snapshots each frame out of the recycled buffer, forwards to `net`,
then `msi-wait`s). Finally registers the NIC.

**Returns:** `'ok` on success; `#f` (logged) on absence, feature rejection, or
MSI-X setup failure. This is the only export — the RX/TX ring helpers are private.

---

## `virtio-gpu-init`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/drivers/virtio-gpu/driver.clp`
- **hash:** 67d66956a959eb89

Entry point for the 2D virtio-gpu display driver: spawns a context that brings the
device to DRIVER_OK, initialises every enabled scanout, and registers a
"Virtio GPU Display" with coredisplay.

**Parameters**
- `display-svc` — the coredisplay service handle (receives
  `(register "Virtio GPU Display" 'unknown ctx scanouts)`).

**Behavior.** **Spawns** a restricted context immediately and returns its handle
(mirroring `virtio-net-init`). The context runs `gpu-bringup` (`pci-find` VID
`0x1AF4` DID `0x1050`; common `virtio-bringup` requesting only VERSION_1 — no
VirGL in 2D mode; sets up the control + cursor queues, DRIVER_OK), reads display
geometry via GET_DISPLAY_INFO, and for each enabled scanout creates a 2D resource,
allocates + paints a framebuffer, attaches it as backing, sets the scanout, and
pushes the first frame (transfer + flush). If a scanout came up it registers with
coredisplay and then parks in a serial recv loop answering `flush` (re-transfer +
flush scanout 0), `get-framebuffer` (reply `(fb w h)`), and `display-info` (a
resize-path stub). The control queue is polled (single-command-in-flight,
request/response), not MSI-driven.

**Returns:** the spawned context handle. The spawned bring-up logs and returns
`#f` internally if the device is absent, rejects features, or no scanout enables.

---

## `make-create-2d`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/drivers/virtio-gpu/cmds.clp`
- **hash:** 96f40f73b5c86f0e

Builds a RESOURCE_CREATE_2D virtio-gpu control command (40 bytes).

**Parameters**
- `res-id` — the resource id to create.
- `fmt` — the pixel format (`GPU-FORMAT-X8R8G8B8` = 4).
- `w`, `h` — resource width/height.

Writes the 24-byte control header (type `0x0101`) plus `resource_id`/`format`/
`width`/`height`. All fields are little-endian (native accessors, **not** the
big-endian network helpers). **Returns** a freshly allocated 40-byte CPU buffer.
Exported so the in-OS self-test can pin the struct offsets without a device.

---

## `make-attach-backing`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/drivers/virtio-gpu/cmds.clp`
- **hash:** 3f7156c96c4dfdeb

Builds a RESOURCE_ATTACH_BACKING command (48 bytes) with a single memory entry.

**Parameters**
- `res-id` — the resource to back.
- `addr` — physical address of the backing buffer (u64).
- `length` — the backing length in bytes.

Writes the header (type `0x0106`), `resource_id`, `nr_entries`=1, and the single
`{addr, length, padding}` entry. **Returns** a 48-byte CPU buffer.

---

## `make-set-scanout`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/drivers/virtio-gpu/cmds.clp`
- **hash:** 6ed6a728bfb1d4d4

Builds a SET_SCANOUT command (48 bytes) pointing a scanout at a resource.

**Parameters**
- `scanout-id` — the scanout (display output) index.
- `res-id` — the resource to scan out.
- `x`, `y`, `w`, `h` — the scanout rectangle.

Writes the header (type `0x0103`), the rect at offset 24, then `scanout_id` and
`resource_id`. **Returns** a 48-byte CPU buffer.

---

## `make-transfer-2d`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/drivers/virtio-gpu/cmds.clp`
- **hash:** 5ff8dd101c7e59ef

Builds a TRANSFER_TO_HOST_2D command (56 bytes) flushing guest backing into the
host resource.

**Parameters**
- `res-id` — the resource id.
- `offset` — byte offset into the backing (u64).
- `x`, `y`, `w`, `h` — the transfer rectangle.

Writes the header (type `0x0105`), the rect at offset 24, `offset`, and
`resource_id`. **Returns** a 56-byte CPU buffer.

---

## `make-flush`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/drivers/virtio-gpu/cmds.clp`
- **hash:** 1b90758bd6203302

Builds a RESOURCE_FLUSH command (48 bytes) presenting a resource rectangle on the
host display.

**Parameters**
- `res-id` — the resource id.
- `x`, `y`, `w`, `h` — the flush rectangle.

Writes the header (type `0x0104`), the rect at offset 24, and `resource_id`.
**Returns** a 48-byte CPU buffer.

---

## `make-display-info-cmd`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/drivers/virtio-gpu/cmds.clp`
- **hash:** e56bbe9d3421cac4

Builds a GET_DISPLAY_INFO command (a bare 24-byte control header, type `0x0100`).

Takes **no arguments**. The reply is a `resp_display_info` struct read with the
`resp-display-*` accessors. **Returns** a 24-byte CPU buffer.

---

## `gpu-resp-type`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/drivers/virtio-gpu/cmds.clp`
- **hash:** 21b6d523bfc5d4e3

Reads the response type code (the u32 at offset 0 of the control header) from a
virtio-gpu response buffer.

**Parameters**
- `resp` — the response buffer.

**Returns** the type code, e.g. `GPU-RESP-OK-NODATA` (`0x1100`) or
`GPU-RESP-OK-DISPLAY-INFO` (`0x1101`). Used to validate that a command succeeded.

---

## `resp-display-enabled?`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/drivers/virtio-gpu/cmds.clp`
- **hash:** 1a7bc3426038ad0b

Tests whether scanout `i` is enabled in a GET_DISPLAY_INFO response.

**Parameters**
- `resp` — the display-info response buffer.
- `i` — the scanout/pmode index (0–15).

Reads the `enabled` u32 in pmode `i` (each pmode is 24 bytes from offset 24).
**Returns** `#t` if non-zero. Drives the bring-up loop's per-scanout init.

---

## `resp-display-width`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/drivers/virtio-gpu/cmds.clp`
- **hash:** 27ec0234d1c45e22

Reads scanout `i`'s pixel width from a GET_DISPLAY_INFO response.

**Parameters**
- `resp` — the display-info response buffer.
- `i` — the scanout/pmode index.

Reads the rect `width` field (offset +8) of pmode `i`. **Returns** the width.

---

## `resp-display-height`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/drivers/virtio-gpu/cmds.clp`
- **hash:** 00f2ce6696c43105

Reads scanout `i`'s pixel height from a GET_DISPLAY_INFO response.

**Parameters**
- `resp` — the display-info response buffer.
- `i` — the scanout/pmode index.

Reads the rect `height` field (offset +12) of pmode `i`. **Returns** the height.

---

## `GPU-RESP-MAX`

- **kind:** lisp-const
- **lang:** lisp
- **source:** `lisp/drivers/virtio-gpu/cmds.clp`
- **hash:** a81215fe193f1c54

Size in bytes (`408`) of the largest virtio-gpu response — a `resp_display_info`
struct (24-byte header + 16 pmodes × 24 bytes).

Used to size the DMA response buffer so any GPU reply fits; all non-display-info
replies are a bare header.

---

## `GPU-RESP-OK-NODATA`

- **kind:** lisp-const
- **lang:** lisp
- **source:** `lisp/drivers/virtio-gpu/cmds.clp`
- **hash:** 481111f14bc96ed6

The virtio-gpu "OK, no data" response type code, `0x1100`.

The expected reply to the create/attach/set-scanout/transfer/flush commands;
`gpu-cmd-ok!` warns when a command answers with anything else.

---

## `GPU-RESP-OK-DISPLAY-INFO`

- **kind:** lisp-const
- **lang:** lisp
- **source:** `lisp/drivers/virtio-gpu/cmds.clp`
- **hash:** 02a3b9dad55be23f

The virtio-gpu "OK, display info" response type code, `0x1101`.

The expected reply type to GET_DISPLAY_INFO; the payload is read with the
`resp-display-*` accessors.

---

## `uhci-init`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/drivers/uhci/driver.clp`
- **hash:** 89d57d5104e1feac

Entry point for the UHCI (USB 1.1) host-controller driver: discovers a PIIX/ICH9
UHCI function, enables it, and spawns a context that resets the HC and serves
coreusb transfer messages while polling its two root ports for hotplug.

**Parameters**
- `usb` — the coreusb service handle; the spawned host-controller context sends it
  `(port-connected (self) port speed)` / `(port-disconnected (self) port)` as
  devices come and go (passing its own handle as the controller coreusb later
  addresses transfers to).

**Behavior.** `find-uhci`s the controller by trying the common QEMU/PC VID/DID
pairs (PIIX3 `8086:7020`, PIIX4 `8086:7112`, the three ICH9 UHCI functions
`8086:2934/2935/2936`) and binding the first present — there is no class-code find
in the substrate. If none is present it logs `[uhci] no controller present` and
returns `#f`. Otherwise it maps the ECAM, enables memory + bus-master, takes the
UHCI register base from the **I/O BAR** (BAR4 — UHCI is port-I/O, accessed via
`sys-io` in/out), and **spawns** a restricted bring-up context (`uhci-bringup`).
The bring-up allocates a 4 KiB 32-bit frame list + a 4 KiB DMA scratch page,
points every frame at one persistent idle control QH, global/HC-resets the
controller and sets Run, then enters the single HC context loop: it drains pending
transfer-request messages (`control` / `interrupt-in` / `bulk`, plus the
`prepare-downstream` / `mark-hub` / `disconnect-dev` housekeeping messages),
building TD chains in the scratch page and polling them to completion (the one
context *is* the serialization the C `ctrl_lock` provided), and every
`POLL-INTERVAL` (250 ms) scans the root ports, resetting + enabling new connects
and notifying coreusb. There is no usable interrupt (no MSI, no parsed INTx GSI),
so completion is read straight from DMA-written TD status and the loop yields via
`sleep`.

**Returns:** the symbol `'uhci-spawned` once the bring-up context is launched, or
`#f` if no controller was found. The bring-up itself never returns (it is the HC
serve loop) and runs fire-and-forget — the caller (boot `system-init`) is not
under the scheduler and cannot block.

---

## `xhci-init`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/drivers/xhci/driver.clp`
- **hash:** 2686f0f450ce503b

Entry point for the xHCI (USB 3) host-controller driver: discovers an xHCI
controller, assigns its BAR if firmware left it unconfigured, and spawns a context
that resets the HC, builds the command/event/transfer rings, sets up MSI, and
serves coreusb transfer messages while polling its ports.

**Parameters**
- `usb` — the coreusb service handle; the spawned host-controller context notifies
  it of port connect/disconnect and receives transfer-request messages back,
  passing its own handle as the controller.

**Behavior.** `find-xhci`s the controller by VID/DID (qemu-xhci `1b36:000d`,
nec-usb-xhci `1033:0194`, and a few Intel PCH xHCIs); absent → logs
`[xhci] no controller present`, returns `#f`. It maps the ECAM, enables memory +
bus-master, and takes the MMIO base from BAR0 — calling `pci-assign-bars` and
re-reading BAR0 if firmware left it zero; a still-absent BAR logs `[xhci] no BAR`
and returns `#f`. Otherwise it **spawns** a restricted bring-up context
(`xhci-bringup`) that maps the MMIO window, reads the capability registers
(CAPLENGTH/RTSOFF/DBOFF/HCSPARAMS), resets the controller, builds the DCBAA,
command ring, event ring (+ ERST), and scratchpad, sets up the per-device MSI,
starts the controller and powers the ports, then enters the single HC context
loop serving transfers and polling ports at `XPOLL-INTERVAL` (250 ms). Unlike
UHCI, xHCI manages addressing itself: on a port connect the driver issues Enable
Slot + Address Device (BSR=1) so EP0 works at the default address, then drives
normal coreusb enumeration, intercepting SET_ADDRESS to issue Address Device
(BSR=0) and recording the coreusb-address→slot mapping. **Event-ring polling:**
the context waits on the per-device MSI (`msi-wait`) to yield while a transfer is
outstanding, but **also polls the event ring directly** so a missed/coalesced
interrupt cannot wedge a transfer; all ring cursors, the event-ring dequeue, the
slot table and per-endpoint rings live in `set!`-able bindings of the bring-up
thunk (one context, so the mutation is race-free). Per-command input contexts and
per-endpoint rings are a bounded leak across enumerations (no `dma-free`).

**Returns:** the symbol `'xhci-spawned` once the bring-up context is launched, or
`#f` on absence / no BAR. The bring-up itself never returns (it is the HC serve
loop).

---

## `usb-hid-init`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/drivers/usb-hid.clp`
- **hash:** cdbe2580d3a0a21b

Entry point for the USB HID boot-protocol class driver: spawns the class-driver
context and registers it with coreusb as the handler for
`bInterfaceClass == HID`.

**Parameters**
- `usb` — the coreusb service handle (receives the
  `(register-class USB-CLASS-HID ctx)` registration).
- `input` — the coreinput service handle; a claimed keyboard registers with it
  (`(register "USB Keyboard")`) and per-key `(event (kbd-down k))` /
  `(event (kbd-up k))` events are sent to it.

**Behavior.** Creates a `serve` context whose state is the list of claimed devices
(`(addr . poll-ctx)` pairs) and registers it with coreusb. On `(probe dev)`
(`hid-on-probe`) it finds an interrupt-IN endpoint (none → logs, does not claim),
reads the interface protocol/number, issues best-effort SET_PROTOCOL (boot) +
SET_IDLE over the control endpoint, registers a keyboard with coreinput if the
protocol is keyboard, and **spawns** a per-device poll context (`hid-poll`) that
repeatedly issues `usb-interrupt-in` (all transfers are messages to the controller
context — the HID driver holds no hardware capability), decoding ≥8-byte
boot-keyboard reports into coreinput key down/up events by diffing against the
previous report. On `(remove addr)` it sends the matching poll context `'stop`
(checked between polls; a `'stop` racing an in-flight transfer can be missed — a
known limitation vs. the C driver's stop/stopped handshake).

**Returns:** the served class-driver context handle (also handed to coreusb in the
`register-class` message).

---

## `usb-hub-init`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/drivers/usb-hub.clp`
- **hash:** 5308d04be90f67c1

Entry point for the USB hub class driver: spawns the class-driver context and
registers it with coreusb as the handler for `bInterfaceClass == Hub`.

**Parameters**
- `usb` — the coreusb service handle (receives `(register-class USB-CLASS-HUB ctx)`
  and, from the per-hub poll contexts, downstream enumerate/disconnect requests).

**Behavior.** Creates a `serve` context (state = list of claimed hubs as
`(addr . poll-ctx)` pairs) and registers it with coreusb. On `(probe dev)`
(`hub-on-probe`) it reads the hub descriptor over the control endpoint (failure →
logs, does not claim), clamps the downstream port count to `HUB-MAX-PORTS` (15),
calls `usb-mark-hub` so the controller routes transfers to devices behind the hub,
powers every port (SET_FEATURE PORT_POWER) and waits for power-good, then
**spawns** a per-hub poll context (`hub-poll`). The poll context closes over the
coreusb handle and, every 200 ms, reads each downstream port's status: on a new
connect it clears the connection-change, resets the port, and — if the port comes
up enabled — calls `usb-enumerate-downstream` (a message to coreusb) at the
detected low/full speed so the device routes to its own class driver; on a new
disconnect it calls `usb-disconnect-downstream`. The class + poll contexts hold no
hardware capability — all control transfers and downstream enumeration are
messages. On `(remove addr)` it sends the matching poll context `'stop`.

**Returns:** the served class-driver context handle (also handed to coreusb).

---

## `usb-storage-init`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/drivers/usb-storage.clp`
- **hash:** 0d42d2e53c904805

Entry point for the USB Mass Storage (Bulk-Only Transport + SCSI) class driver:
spawns the class-driver context and registers it with coreusb as the handler for
`bInterfaceClass == Mass Storage`.

**Parameters**
- `usb` — the coreusb service handle (receives
  `(register-class USB-CLASS-MASS-STORAGE ctx)`).
- `storage` — the corestorage service handle; a probed device with usable capacity
  registers a block device with it (`(register-blockdev 'usb0 bsize bcount srv)`)
  and the per-device server answers its `(read lba count reply)` /
  `(write lba count data reply)` requests.

**Behavior.** Creates a `serve` context (state = list of claimed devices as
`(addr . server-ctx)` pairs) and registers it with coreusb. On `(probe dev)`
(`stor-on-probe`) it finds the bulk IN and OUT endpoints (either missing → logs,
does not claim) and **spawns** a per-device block server (`start-block-server`).
The server runs the SCSI/BBB machinery: each command is a CBW (out) → optional
data → CSW (in) sequence of three bulk transfers, with the CSW validated against
its signature and echoed tag. It runs INQUIRY (logged) and READ CAPACITY at
bring-up; if the medium has capacity it registers `usb0` with corestorage and
serves block requests, otherwise it parks until `(stop)`. Reads/writes are chunked
`MAX-BLK` (4 = 2 KiB) blocks per BBB command via READ(10)/WRITE(10). **The stash:**
the server interleaves two streams on its own mailbox — block requests from
corestorage and transfer completions from the controller — so the transfer-wait
`await` stashes any non-`complete` message into a FIFO that the main loop drains
before recv-ing fresh, the same single-context message-IO serialization the cardfs
port needed. On `(remove addr)` it sends the matching server `(stop)` (a list, so
the server's `(car req)` stays valid) so the context ends cleanly.

**Returns:** the served class-driver context handle (also handed to coreusb).
