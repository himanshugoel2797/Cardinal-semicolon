# ahci

> A SATA AHCI block-device driver that discovers a single ICH9-compatible HBA, brings up the first live port, and registers the device with [corestorage](../servers/corestorage.md).

| | |
|---|---|
| **Source** | `lisp/drivers/ahci.clp` + `lisp/drivers/ahci/{hba,port,ata,driver}.clp` |
| **Kind** | driver |
| **Bound by** | `lisp/init.clp` — called unconditionally in `system-init`; `ahci-init` self-gates on `(pci-find #x8086 #x2922)` (QEMU ICH9 only) and returns if no device is present |
| **Registers with** | [corestorage](../servers/corestorage.md) via `register-blockdev` message |
| **Capabilities** | `sys-mmio` (`mmio-map`, `dma-alloc`, `dma-alloc-32`), `sys-pci` (`pci-find`, `pci-assign-bars`, `pci-setup-msi`, `msi-count`, `msi-wait`), `driver-util` |

## Overview

`ahci` is a SPLIT module, split across four component-private include files (`hba`, `port`, `ata`, `driver`) compiled into one `(define-module ahci ...)`. It owns three layers of hardware state:

- **HBA global registers** — CAP/CAP2 decode, BIOS/OS handoff (gated on `CAP2.BOH`), GHC reset, AHCI enable, and the PI-bitmap port scan.
- **Per-port DMA regions** — command list (1 KB), received-FIS buffer (256 B), and command table (512 B). A fourth caller-owned buffer holds sector data.
- **ATA command machinery** — H2D register FIS construction, PRDT setup, `PxCI`-poll + `msi-wait`-yield completion, and IDENTIFY/READ DMA EXT/WRITE DMA EXT.

The driver spawns two long-lived Lisp contexts: a one-shot bring-up context that resets the HBA, scans ports, runs IDENTIFY, and registers the device; and the permanent driver-request context that processes `read`/`write` messages from corestorage. Both contexts yield the core (via `wait-until`, `sleep`, and `msi-wait`) instead of busy-spinning.

**Class-binding limitation.** `pci-find` matches vendor/device ID only (`8086:2922`). A class-code match (AHCI class `01/06`) that would bind any AHCI controller is a noted future substrate addition; it is not built here. On hardware with a non-ICH9 AHCI controller the driver silently skips initialization.

## Initialization

`init.clp` calls `ahci-init` once, passing the corestorage handle it obtained from `start-storage-service`.

```scheme
(ahci-init storage)   ; → 'ahci-spawned on success, #f if no device present
```

`ahci-init` is intentionally fire-and-forget: it `pci-find`s the controller, maps the ABAR, and immediately **spawns** a bring-up context (`ahci-bringup`) before returning. `init.clp` does not await completion. This matches the pattern used by `virtio-net-init` and `virtio-gpu-init`.

### Bring-up sequence (`ahci-bringup`)

1. Read `CAP.S64A` to decide between `dma-alloc` (64-bit phys) and `dma-alloc-32` (phys < 4 GB).
2. `hba-handoff!` — request OS ownership via `BOHC.OOS`; waits up to 3 s total for `BIOS-owned` and `BIOS-busy` bits to clear. Skipped entirely when `CAP2.BOH = 0` (the QEMU ICH9 does not implement the handshake).
3. `hba-reset!` — sets `GHC.AE` then `GHC.HR`, polls until `GHC.HR` self-clears (≤ 1 s), then re-asserts `GHC.AE`.
4. `hba-ports` — reads PI bitmap, returns the list of implemented port indices 0–31.
5. `find-live-port` — iterates the port list; for each port: idles it, waits for `PxSSTS.DET = 3` (link present), clears `PxSERR`/`PxIS`, programs `CLB`/`FB`, enables `FRE` then `ST`, waits for `BSY|DRQ` clear. Returns `(port regions)` for the first port that succeeds.
6. `identify` — issues ATA IDENTIFY (0xEC) using a poll-only context (MSI not yet set up).
7. `pci-setup-msi ecam` — installs an MSI handler; the returned slot is used by `msi-wait`/`msi-count` in `issue!`.
8. Allocates the shared data DMA buffer (`DATA-SECTORS * 512` bytes = 4 KB).
9. Arms `PxIE` (`PxIE-MASK = 0x7DC000FF`) and `GHC.IE` **after** the driver context exists (IRQ-last ordering).
10. Runs `ahci-smoke` (read LBA 0 + write/readback scratch at `sectors - 8`).
11. Sends `(register-blockdev 'ahci0 512 sectors driver-ctx)` to corestorage.

### ABAR mapping

ABAR is BAR5 of the PCI config space. If firmware left the BAR unassigned (base address 0), `pci-assign-bars` is called and BAR5 is **re-read** after — the return value of `pci-assign-bars` is the first BAR (BAR0, a legacy IDE I/O range on the ICH9), not the ABAR, and must be discarded.

```scheme
(let ((abar-phys (let ((b (bar-base cfg ABAR-BAR)))
                   (if (= b 0)
                       (begin (pci-assign-bars ecam) (bar-base cfg ABAR-BAR))
                       b))))
  (mmio-map abar-phys #x1100))   ; 0x100 HBA globals + 32 * 0x80 port regs
```

## Message protocol

The driver-request context (`make-driver-ctx`) sits behind corestorage; callers do not send to it directly. corestorage routes `read`/`write` requests from the OS and forwards them to the driver handle it receives at registration.

### `:read`

```scheme
(send driver-ctx (list 'read lba count reply))
```

- **`lba`** — 48-bit starting logical block address.
- **`count`** — number of 512-byte sectors to read (capped at `DATA-SECTORS = 8`).
- **`reply`** — reply handle; the driver sends back `(list 'complete status bytes)`.
  - **`status`** — `0` on success, `-1` on error or if `count > DATA-SECTORS`.
  - **`bytes`** — on success, a fresh copy of the DMA buffer slice (`count * 512` bytes, copied via `copy-bytes`); `#f` on error.

### `:write`

```scheme
(send driver-ctx (list 'write lba count data reply))
```

- **`lba`** — 48-bit starting logical block address.
- **`count`** — number of sectors to write (capped at `DATA-SECTORS = 8`).
- **`data`** — caller-supplied bytes buffer; copied into the DMA buffer via `bytes-copy-into!`.
- **`reply`** — reply handle; the driver sends back `(list 'complete status)`.
  - **`status`** — `0` on success, `-1` on error or if `count > DATA-SECTORS`.

> **Serialization.** corestorage routes all I/O through a single driver context (one mailbox). No concurrent commands are in flight. `DATA-SECTORS = 8` (4 KB) bounds each request to one PRDT entry.

## Exported functions

These are exported from the module and may be imported by test harnesses or other code.

### `(ahci-init storage)`

Entry point called by `init.clp`. Discovers the ICH9 AHCI controller via `pci-find`; if absent, logs `[ahci] no device present` and returns `#f`. On success, spawns the bring-up context and returns `'ahci-spawned`.

### `(fis-build! fis command lba count device)`

Writes a 20-byte H2D Register FIS into `fis` (a bytes buffer). Zeroes the buffer first (the command table may be reused). `lba` is a 48-bit integer laid out a byte at a time in the on-wire positions (bytes 4–6 low, bytes 8–10 high). `count = 0` means 65 536 sectors for DMA-EXT commands. Returns `fis`.

```scheme
(fis-build! cmdtbl ATA-READ-EXT lba count #x40)
```

### `(prdt-set! ctbl data-phys len s64a?)`

Writes PRDT entry 0 at offset `PRDT-OFF` (`0x80`) inside a command-table buffer. Sets `DBA`/`DBAU` from `data-phys` (upper dword zeroed when `s64a?` is `#f`), `DBC = len - 1` with bit 31 (interrupt-on-completion) forced set. `len` must be even and ≤ 4 MB. Returns `ctbl`.

### `(cmdhdr-set! cmdlist ctbl-phys cfl-dwords write? prdtl)`

Writes command-list slot 0 (the 32-byte command header at offset 0). `cfl-dwords` is the FIS length in dwords (always 5 for a 20-byte H2D FIS). `write?` sets bit 6 (host-to-device direction). `prdtl` is the PRDT entry count (always 1 here). Programs `CTBA`/`CTBAU` from `ctbl-phys`.

### `(id-sector-count buf)`

Parses the total addressable sector count from a 512-byte IDENTIFY data buffer. If word 83 bit 10 (`LBA48 supported`) is set, returns the 48-bit count from words 100–103; otherwise the 28-bit LBA count from words 60–61.

### `(id-model buf)`

Returns the model string from IDENTIFY words 27–46 (40 ASCII bytes) as a Scheme string, byte-swapping each word to reverse the ATA word-byte ordering, and trimming trailing spaces and NULs.

### `(id-word buf w)`

Returns IDENTIFY word `w` (0-indexed) as a 16-bit integer. Words are little-endian 16-bit values at byte offset `2 * w`.

### Constants

| Constant | Value | Meaning |
|---|---|---|
| `ATA-IDENTIFY` | `#xEC` | ATA IDENTIFY DEVICE command |
| `ATA-READ-EXT` | `#x25` | READ DMA EXT command |
| `ATA-WRITE-EXT` | `#x35` | WRITE DMA EXT command |
| `FIS-OFF` | `#x00` | Offset of the H2D FIS within the command table |
| `PRDT-OFF` | `#x80` | Offset of PRDT entry 0 within the command table |

## Key internal structures

### HBA context tuple `ctx`

`issue!` and the higher-level `read-sectors`/`write-sectors` functions thread an opaque list:

```scheme
(list abar port regions s64a? msi)
; abar    — mmio-mapped bytes buffer of the AHCI ABAR (≥ 0x1100 bytes)
; port    — integer port index (0–31)
; regions — (list cmdlist-buf rxfis-buf cmdtbl-buf) DMA buffers for this port
; s64a?   — boolean: controller supports 64-bit DMA addresses
; msi     — MSI slot handle from pci-setup-msi, or #f (poll-only)
```

Accessor helpers: `ahci-ctx-abar`, `ahci-ctx-port`, `ahci-ctx-regions`, `ahci-ctx-s64a`, `ahci-ctx-msi`.

### DMA regions

| Buffer | Size | Contents |
|---|---|---|
| Command list | 1 024 B | 32 × 32-byte command headers; only slot 0 is used |
| Received FIS | 256 B | HBA writes D2H/PIO/DMA-setup FISes here |
| Command table | 512 B | H2D FIS at offset 0 (`FIS-OFF`); PRDT at offset 128 (`PRDT-OFF`) |
| Data buffer | `DATA-SECTORS × 512` B | Sector payload; owned by the driver context |

### Per-port registers

All port registers are accessed as `0x100 + 0x80 × port + field`. Key fields:

| Name | Offset | Description |
|---|---|---|
| `PxCLB`/`PxCLBU` | `0x00`/`0x04` | Command-list base (lo/hi) |
| `PxFB`/`PxFBU` | `0x08`/`0x0C` | Received-FIS base (lo/hi) |
| `PxIS` | `0x10` | Interrupt status (write 1 to clear) |
| `PxIE` | `0x14` | Interrupt enable |
| `PxCMD` | `0x18` | Command + status (`ST`/`FRE`/`FR`/`CR`) |
| `PxTFD` | `0x20` | Task-file data (`BSY`/`DRQ`/`ERR`) |
| `PxSSTS` | `0x28` | SATA status (`DET` bits 3:0) |
| `PxSERR` | `0x30` | SATA error (write all-ones to clear) |
| `PxCI` | `0x38` | Command issue; bit 0 = slot 0; clears on completion |

## Notes / gotchas

**All waits yield.** Every reset and completion wait (`wait-until`, `sleep`, `msi-wait`) runs inside a spawned context so it deschedules the core rather than busy-spinning. The bring-up context is spawned precisely so these yields have somewhere to go — none of it runs on an early-boot `cli()` path.

**MSI as a yield hint, not the truth.** `issue!` treats `PxCI` bit-0 clearing as the authoritative completion signal. `msi-wait` is called only to yield the core between polls (up to 50 ms per wait), bounded by a 3 s overall deadline. A missed or coalesced MSI therefore never wedges a command; `msi-count` is used only to log whether an interrupt carried a given completion.

**IRQ-last ordering.** `PxIE` and `GHC.IE` are armed only after `make-driver-ctx` returns and the driver mailbox exists. Enabling the interrupt mask before the handler is wired can cause an edge-triggered MSI to fire and be lost, wedging the interrupt for that device (a PCI MSI is sent-and-forgotten; there is no re-delivery).

**DMA allocation leaks on rejected ports.** `find-live-port` allocates three DMA regions per candidate port and keeps only the winner's. Regions for ports whose link does not come up are never freed (there is no `dma-free` primitive — foreign DMA buffers are not GC'd). At boot this is a bounded, small loss (~1.8 KB per rejected port).

**`DATA-SECTORS = 8` cap.** Each request is bounded to 8 sectors (4 KB) to fit inside a single PRDT entry (max 4 MB). corestorage bounds `lba + count` against the device block count but does not bound `count` alone; the cap is enforced inside the driver context.

**BIOS/OS handoff is conditional.** The QEMU ICH9 AHCI controller does not set `CAP2.BOH`, so `hba-handoff!` detects this and returns `'no-handoff` without touching `BOHC`. On firmware that does implement the handshake the driver sets `BOHC.OOS` and waits up to 3 s total for `BOS` and `BB` to clear.

**`pci-assign-bars` return value must be discarded.** The ICH9 has legacy IDE I/O ranges in BAR0–4; `pci-assign-bars` returns the first BAR base, not ABAR. Always discard the return value and re-read BAR5 via `bar-base cfg ABAR-BAR` after assignment.

**Port scan instead of assuming port 0.** A QEMU q35 machine exposes several empty AHCI ports before the populated one. `find-live-port` scans the PI bitmap and tries each implemented port until one's link check passes, rather than assuming the disk is on port 0.
