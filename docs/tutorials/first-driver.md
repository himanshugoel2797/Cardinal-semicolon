# Write your first driver

*Build a minimal Lisp PCI NIC driver end-to-end — from module skeleton to frame delivery — using the RTL8139 driver as a running example.*

This tutorial walks through every layer of a Cardinal; Lisp driver: the module
skeleton, how the boot policy discovers and wires it, the hardware bring-up
sequence, registration with a Core\* service, and the one rule that bites
everyone the first time.  By the end you will understand how `lisp/drivers/rtl8139.clp`
works and be ready to adapt the same pattern for your own device.

!!! note "No CMake changes needed"
    Lisp drivers are `.clp` files under `lisp/drivers/`.  The build system
    packages **every** `.clp` file in that tree into the initrd automatically.
    You do not touch `CMakeLists.txt` or `loadscript.txt`.  The only files you
    need to edit are your new `.clp` and `lisp/init.clp`.

---

## 1. The shape of a driver module

Every Cardinal; Lisp driver is a **module** — a named unit that declares what it
exports, what authority it needs, and then defines its code.  The RTL8139 driver
opens with:

```scheme
(define-module rtl8139
  (export rtl8139-init rx-parse-one tx-fill! rx-extract!)
  (import sys-mmio sys-pci driver-util)
```

`define-module` takes:

- a **name** (`rtl8139`) — used by `import` elsewhere.
- an **`export`** list — the symbols callers can see.  The init function is
  always exported; pure helpers (`rx-parse-one`, `tx-fill!`) are also exported
  here so unit tests can reach them without hardware.
- an **`import`** list — the capability modules this driver is *allowed* to use.
  The VM enforces this: if `sys-mmio` is not listed, the driver cannot call
  `mmio-map` or `dma-alloc-32`, even if the boot context holds that authority.

The capabilities a NIC driver typically needs are:

| Module | Grants access to |
|---|---|
| `sys-mmio` | `mmio-map`, `dma-alloc-32` / `dma-alloc` (MMIO mapping + DMA allocation) |
| `sys-pci` | `pci-find`, `pci-find-all`, `pci-assign-bars`, `pci-setup-msi` (PCI config-space + MSI) |
| `driver-util` | `bar-base`, `pci-enable-mem-bus-master!`, `wait-until`, `make-cell`, `copy-bytes`, … |

The RTL8139 has no MSI capability, so it only needs `sys-mmio sys-pci driver-util`.
A device with MSI-X (like `virtio-net`) would also list `sys-pci` for
`pci-setup-msi`.

!!! tip "Capabilities are lexically captured, not re-checked at runtime"
    The driver captures `mmio-map`, `dma-alloc-32`, etc. into its own closures
    at load time.  The spawned contexts it creates later — the TX loop, the RX
    poll loop — receive no capability grant of their own (`spawn-restricted '()`).
    They can still call `mmio-map` because they close over the mapping already
    built during init, not because they hold the raw primitive.  A compromised
    driver loop cannot `(import sys-pci)` to probe new hardware.

---

## 2. How `init.clp` discovers and binds it

`lisp/init.clp` is the **sole device binder**.  There is no `devices.txt`, no
`CoreDriver` C module.  Every "which driver goes to which hardware" decision
lives here, in plain Lisp.

For NIC drivers, `init.clp` defines a tiny local helper and calls it once per
supported NIC family:

```scheme
(let ((nics 0))
  (define (bring-up vid did drv)
    (for-each (lambda (ecam) (drv net ecam) (set! nics (+ nics 1)))
              (pci-find-all vid did)))
  (bring-up #x1af4 #x1041 virtio-net-init)
  (bring-up #x10ec #x8168 rtl8169-init)
  (bring-up #x10ec #x8139 rtl8139-init)
  (if (= nics 0) (begin (display "[init] no supported NIC") (newline))))
```

`pci-find-all` returns a list of ECAM (Enhanced Configuration Access Mechanism)
pointers — one per PCI function matching the vendor/device ID pair.  The
`for-each` iterates over all of them, calling the driver's init function with
`net` (the corenetwork handle) and each device's ECAM.  This means two
RTL8139s in the same machine each get their own driver instance without any
extra code.

To bind **your** driver, you add one `bring-up` call with your device's PCI
vendor and device IDs, and your init function name.

---

## 3. Inside init: mapping the device, allocating DMA, bringing up hardware

The driver's init function receives two arguments:

```scheme
(define (rtl8139-init net dev-ecam)
  …)
```

- `net` — the corenetwork service handle (obtained earlier in `system-init` by
  `start-network-service`).
- `dev-ecam` — the raw physical address of this device's 4 KB PCI config space,
  as a `bytes` object returned by `pci-find-all`.

### 3a. Map the config space and enable the device

```scheme
(let ((cfg (mmio-map ecam #x1000)))
  (pci-enable-mem-bus-master! cfg)
```

`mmio-map` from `sys-mmio` creates a byte-addressable view of the config space.
`pci-enable-mem-bus-master!` (from `driver-util`) sets bits 1 and 2 of the PCI
COMMAND register — memory-space decoding and bus mastering — the two bits every
DMA-capable device needs.

### 3b. Read (or assign) the BAR

```scheme
(let* ((b0 (bar-base cfg 1))
       (base (if (= b0 0)
                 (begin (pci-assign-bars ecam) (bar-base cfg 1))
                 b0)))
```

`bar-base` (from `driver-util`) decodes the BAR's base physical address,
handling both 32-bit and 64-bit BARs.  If firmware left it unassigned (reads as
zero — common for devices that were not enumerated before the OS started),
`pci-assign-bars` places all BARs for the device and opens every bridge window
up to the root bus.  After that, re-read the BAR.

The register window itself is then mapped:

```scheme
(let ((regs (mmio-map base #x100)))
```

### 3c. Reset the device and wait for it to settle

```scheme
(bytes-u8-set! regs CONFIG1 0)          ; power on
(bytes-u8-set! regs TX-CMD CMD-RST)     ; software reset
(wait-until
  (lambda () (= 0 (bitwise-and (bytes-u8-ref regs TX-CMD) CMD-RST)))
  100000000)                            ; 100 ms timeout
```

`wait-until` (from `driver-util`) polls a predicate, yielding between checks
with a `sleep` so other contexts can run.  It returns `#t` if the condition
becomes true within the timeout, `#f` if it times out (at which point the driver
logs an error and returns `#f` rather than hanging boot).

### 3d. Allocate DMA buffers

```scheme
(let ((rxbuf  (dma-alloc-32 RX-BUF-SIZE))
      (txbufs (alloc-tx-bufs)))   ; four TX slots via dma-alloc-32
```

`dma-alloc-32` allocates physically-contiguous memory below the 4 GB boundary —
mandatory for the RTL8139, which has a 32-bit DMA address bus.  For devices with
a full 64-bit DMA address space, use `dma-alloc` instead.

The driver checks that the returned physical addresses are actually below 4 GB:

```scheme
(if (or (not rxbuf) (>= (bytes-phys rxbuf) #x100000000) (not txbufs))
    (begin (display "[rtl8139] DMA buffer alloc failed (need <4GB)") (newline) #f)
```

After allocating the buffers, the driver programs their physical addresses into
the device's RX and TX descriptor registers, sets receive/transmit configuration,
and enables RX+TX — the device is now live.

---

## 4. Registering with corenetwork and the message contract

Once the hardware is running, the driver spawns two long-lived contexts — one
for TX, one for RX — and registers both with the network service.

### The TX context

```scheme
(let ((tx-ctx
        (spawn-restricted '() (lambda ()
          (let loop ()
            (let ((m (recv)))
              (if (eq? (car m) 'tx)
                  (begin
                    (tx-fill! regs txbufs free-cell (cadr m) (caddr m))
                    (if (> (length m) 3) (send (nth m 3) (list 'tx-done)))))
              (loop)))))))
```

`spawn-restricted '()` creates a new context with **no** capability grant — it
cannot import new modules.  It loops on `recv`, waiting for `'tx` messages.
When one arrives, `tx-fill!` copies the frame into a TX slot and kicks the
device.  If a reply-context handle was included (the optional fourth element),
it sends `'tx-done` back.

The TX context is the handle corenetwork will use every time it needs to send a
frame out this NIC.

### The RX context

```scheme
(spawn-restricted '() (lambda ()
  (let loop ()
    (rx-extract! regs rxbuf off-cell
      (lambda (foff flen)
        (send net (list 'rx (copy-bytes rxbuf foff flen) flen))))
    (sleep RX-POLL-NS)
    (loop))))
```

The RX context drains every frame currently in the receive ring and forwards each
one to corenetwork.  Then it calls `(sleep RX-POLL-NS)` — `sleep` yields the
scheduler, so the core runs other contexts during the 1 ms interval.

Note `copy-bytes`: the receive ring is a recycled buffer — the device will
overwrite it on the next receive.  The frame data **must** be snapshot-copied
into a fresh owned buffer before forwarding; the `bytes` object sent to
corenetwork has its own lifetime independent of the ring.

### Registration

```scheme
(send net (list 'register-nic mac tx-ctx))
```

This single message registers the NIC with the network stack.  `mac` is a
6-element list of byte values read from the device's MAC address registers.
`tx-ctx` is the handle of the TX context spawned above.  From this point on,
corenetwork owns the framing; the driver is pure transport.

See [corenetwork](../servers/corenetwork.md) for the complete driver-facing
message API, including the `'rx` dispatch and the `'set-address` / `'dhcp-start-all`
address configuration messages.

---

## 5. The one rule: never hold a lock across the RX forward

When the RX context sends `(send net (list 'rx frame len))`, corenetwork
processes the frame **synchronously in the driver's send**: ARP replies, ICMP
echo replies, and UDP replies all re-enter the TX path before the `send` returns.

If your driver's RX context holds any lock (a mutex, a `cell` that serialises
slot access, a spin held during frame extraction) when it calls `send net (rx …)`,
and the resulting TX call needs to acquire *the same lock*, you have an
instant self-deadlock — corenetwork calls back into your TX context on the same
logical thread.

The RTL8139 driver avoids this structurally: the `rx-extract!` callback captures
only the ring offset cell and the rxbuf, neither of which the TX context touches,
so there is no lock to hold.  If you add shared state between your TX and RX
paths, move it out of scope — or release the lock — before the `(send net …)`
call.

!!! warning "This rule applies to any Core\* service that can call back into your driver"
    The pattern is not specific to networking.  Any service that processes an RX
    message and immediately calls back into your driver's TX context has the same
    re-entrancy shape.  Design your driver so the RX forward is always done with
    no exclusive state held.

See the **Driver gotcha — RX-handler locking** section in `CLAUDE.md` for the
full explanation, and the [rtl8139 driver reference](../drivers/rtl8139.md) for
this driver's specific notes on the subject.

---

## 6. Rebuild and boot to see it bind

Because Lisp drivers are auto-packaged into the initrd, the rebuild is just:

```bash
source ./scripts/devenv/activate.sh
./scripts/build.sh
cmake --build build --target image
./scripts/run-qemu.sh
```

You do not re-run `cmake -S . -B build` unless you added a new *C* source file.
The glob picks up new `.clp` files the next time `scripts/build.sh` runs; it
calls `cmake` for you.

To exercise the RTL8139 driver specifically, pass `-device rtl8139` on the QEMU
command line (the default QEMU machine uses `virtio-net-pci`).  On a successful
bind you will see on the COM1 console:

```
[rtl8139] registered with network stack
```

If the PCI device is absent, `pci-find-all` returns an empty list and the
`bring-up` `for-each` iterates zero times — no error, no bind, no log.

For general build/run instructions see the [build tutorial](build-and-boot.md) and the
`scripts/run-qemu.sh` header for the full set of environment knobs (`MACHINE`,
`GPU`, `MEM`, `ACCEL`, `SCREENSHOT`, `TIMEOUT`, …).

---

## Next steps

- [Add a PCI driver (in-depth how-to)](../guides/add-a-pci-driver.md) — covers
  MSI-X setup, multi-queue virtqueues, `virtio-bringup`, BAR assignment edge
  cases, and testing against a real or emulated device.
- [Capabilities and the sandbox](../concepts/capabilities-and-sandbox.md) —
  explains how `define-module … (import …)` gates authority, what
  `spawn-restricted` means for a driver loop, and the relationship between the
  Lisp VM sandbox and hardware privilege levels.
