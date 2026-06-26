# Add a PCI driver

*How to write a new PCI device driver in Cardinal Lisp, from a blank file to a
registered, interrupt-driven service.*

This guide assumes a working build (see `CLAUDE.md` §"Toolchain & build") and
is the in-depth reference behind the introductory tutorial. It walks every stage
of the driver pattern as it actually appears in `lisp/drivers/rtl8169.clp`,
`lisp/drivers/virtio-net.clp`, `lisp/drivers/ahci/driver.clp`, and
`lisp/drivers/hdaudio.clp`.

---

## 1. Driver module skeleton

A Cardinal driver is a Lisp *module* file under `lisp/drivers/`. The initrd is
built by globbing that directory, so **no CMake change is needed** — drop a
`.clp` file there and it is automatically packaged and loadable at boot.

```scheme
;; lisp/drivers/mydev.clp
(define-module mydev
  (export mydev-init)          ; entry point called from init.clp
  (import sys-mmio sys-pci driver-util)

  ;; ... register constants, helpers ...

  (define (mydev-init svc ecam)
    ...))
```

The three imports cover the common driver surface:

| Module | What it unlocks |
|--------|----------------|
| `sys-mmio` | `mmio-map`, `mmio-map-wc`, `mmio-map-wb`, `dma-alloc`, `dma-alloc-wb`, `dma-alloc-32` |
| `sys-pci` | `pci-find`, `pci-find-all`, `pci-find-class`, `pci-find-class-all`, `pci-setup-msi`, `msi-count`, `msi-wait`, `pci-assign-bars` |
| `driver-util` | `bar-base`, `pci-enable-mem-bus-master!`, `wait-until`, `wait-until-spin`, `make-cell`/`cell-ref`/`cell-set!`, byte-order helpers, `serve`, `reply-to` |

Capabilities are lexical: a context that never `import`s `sys-mmio` simply
cannot name `dma-alloc` or `mmio-map`. Restricted child contexts spawned by the
driver (`spawn-restricted '() ...`) inherit whatever the parent already
imported at the time of spawn — they cannot acquire new authority later. This is
the W7 capability posture; see `lisp/init.clp` for the full picture.

!!! note "Split multi-file modules"
    A driver spanning several files can use `(include "ahci/hba.clp")` etc.
    inside the `define-module` body — see `lisp/drivers/ahci/`. The loader
    searches a path list; the folder is not part of the module name.

---

## 2. Getting the device

### The ECAM handoff

`init.clp` is the sole device binder. It uses `pci-find*` to discover hardware
and passes the resulting ECAM physical address to your driver's init function.
Your driver never calls `pci-find` itself — it receives the ECAM as a
parameter. The four discovery helpers (all from `sys-pci`):

```scheme
;; Exactly one device by VID/DID — returns the ECAM phys addr, or #f.
(pci-find VID DID)

;; All devices with matching VID/DID — returns a list of ECAM phys addrs.
(pci-find-all VID DID)

;; First device matching a PCI class/subclass — for class-generic drivers
;; like hdaudio (class 0x04, subclass 0x03 matches any HDA controller).
(pci-find-class BASE-CLASS SUBCLASS)

;; All devices matching a class/subclass.
(pci-find-class-all BASE-CLASS SUBCLASS)
```

### Mapping config space and enabling the device

The ECAM is 4 KiB of PCI config space. Map it first, then enable memory-space
decode and bus mastering *before touching any BAR* — with mem-decode off every
register read returns `0xFF` (this was the root cause of the rtl8169 reset
timeout, documented in `notes/drivers/rtl8168-bringup.md`):

```scheme
(define (mydev-init svc ecam)
  (if (not ecam)
      (begin (display "[mydev] no device") (newline) #f)
      (let ((cfg (mmio-map ecam #x1000)))
        ;; MUST come before any BAR/MMIO access.
        (pci-enable-mem-bus-master! cfg)   ; from driver-util
        ...)))
```

`pci-enable-mem-bus-master!` sets PCI COMMAND bits 1 (memory space) and 2 (bus
master), using `bytes-u16-ref`/`bytes-u16-set!` on the config buffer.

### Reading BARs

`bar-base` (from `driver-util`) decodes a BAR's base physical address,
handling both 32-bit and 64-bit memory BARs:

```scheme
(let ((base (bar-base cfg 0)))   ; BAR0; adjust index for your device
  (if (= base 0)
      (begin (display "[mydev] no BAR") (newline) #f)
      (let ((regs (mmio-map base #x1000)))
        ...)))
```

The BAR index matches the hardware spec (e.g. rtl8169 uses BAR2, AHCI uses
BAR5, HD Audio uses BAR0).

### pci-assign-bars: when firmware leaves BARs unconfigured

Devices that firmware never uses at boot — a discrete NIC behind a PCIe root
port, or the on-board RTL8111G on the AtomicPi — may have unassigned BARs
(base reads back as `0`) and a closed bridge window. `pci-assign-bars` handles
both: it sizes and places the unassigned memory BARs, then walks every bridge
between the device and the root bus and opens the forwarding window.

The pattern: read BAR, self-assign if zero, re-read:

```scheme
(let ((base (let ((b (bar-base cfg ABAR-BAR)))
              (if (= b 0)
                  (begin (pci-assign-bars ecam) (bar-base cfg ABAR-BAR))
                  b))))
  ...)
```

Note that `pci-assign-bars` takes the *ECAM physical address* (the value init
handed you), not the mapped `cfg` buffer. Its return value is the *first* BAR
(e.g. a legacy I/O range on the ICH9 AHCI) — discard it and re-read the
specific BAR you need, as ahci and rtl8169 both do.

!!! warning "D3hot"
    Some devices (notably the RTL8111G on cold-boot firmware) sit in D3hot: PCI
    config space is readable but MMIO BARs are dead until a D3 → D0 transition.
    Walk the capability list to the Power Management capability (id `0x01`) and
    clear the two PowerState bits in PMCSR; allow 10 ms to settle. See
    `power-on-d0` in `lisp/drivers/rtl8169.clp` for the exact pattern.

---

## 3. DMA

Cardinal Lisp exposes three DMA allocators (all from `sys-mmio`):

| Primitive | Memory type | Physical constraint |
|-----------|------------|---------------------|
| `dma-alloc size` | Uncached (UC) | No constraint; 64-bit phys OK |
| `dma-alloc-32 size` | Uncached (UC) | Guaranteed < 4 GB |
| `dma-alloc-wb size` | Write-back (WB) | No constraint; used for virtio-gpu scanout |

Use `dma-alloc-32` for controllers that cannot address above 4 GB (many legacy
AHCI / HDA controllers report `!S64A` or have 32-bit CORB/RIRB base registers):

```scheme
;; CORB + RIRB rings must be < 4GB on a controller that reports !64OK.
(let ((ring (dma-alloc-32 ring-bytes)))
  ...)

;; RTL8168 supports 64-bit DMA; plain dma-alloc is fine.
(let ((rxring (dma-alloc (* NRX DESC-SIZE)))
      (rxbuf  (dma-alloc (* NRX PKT-SIZE))))
  ...)
```

`bytes-phys` retrieves the physical address of any DMA-allocated buffer:

```scheme
(bytes-phys ring)   ; -> a fixnum, the physical address
```

Ordinary `make-bytes` buffers are GC-managed heap objects; `bytes-phys` on
them returns `0` (not a real DMA address). Always use `dma-alloc*` for hardware
ring/buffer memory.

!!! note "No dma-free"
    There is no `dma-free` primitive; DMA buffers are never collected. Bring-up
    is one-shot at boot, so leaked regions on failed ports (e.g. AHCI port
    probing) are a bounded, small loss.

---

## 4. Interrupts

### Setting up MSI/MSI-X

`pci-setup-msi` (from `sys-pci`) walks the device's PCI capability list, finds
the MSI or MSI-X capability, allocates a kernel interrupt vector, programs the
MSI table, and enables the capability. It returns an opaque MSI handle on
success, or `#f` if no MSI capability exists or no vector is free:

```scheme
(let ((msi (pci-setup-msi ecam)))
  (if (not msi)
      (begin (display "[mydev] MSI setup failed") (newline) #f)
      ...))
```

The handle is used with two primitives:

- `(msi-count msi)` — returns a monotonically increasing counter, incremented
  each time an MSI fires. Reading it without waiting is safe from any context.
- `(msi-wait msi seen)` — deschedules the current context until
  `(msi-count msi)` exceeds `seen`, then returns. Must be called from a
  scheduled context (i.e. a spawned lambda, not the top-level boot eval).

### The RX pump pattern

Both virtio-net and rtl8169 use the same pump loop: check first, wait only if
nothing is pending. This avoids losing an MSI that fired between the last
service pass and the wait:

```scheme
(spawn-restricted '()
  (lambda ()
    (let loop ((seen (msi-count msi)))
      ;; Service whatever is pending.
      (service-ring! ...)
      ;; If count advanced while we were servicing, loop immediately.
      ;; Otherwise park until the next MSI.
      (if (> (msi-count msi) seen)
          (loop (msi-count msi))
          (begin (msi-wait msi seen) (loop (msi-count msi)))))))
```

### The ordering rule: register handler BEFORE enabling device interrupts

This is the single most important sequencing constraint. An edge-triggered MSI
fires **once** when the device first asserts its interrupt. If that edge arrives
before your pump context exists and `msi-wait` is armed, the edge is lost and
the interrupt never re-fires — wedging the driver for the whole boot session
(intermittent on traffic timing, but reproducible; see
`notes/drivers/rtl8168-bringup.md` §"Interrupt-enable race").

The correct order is:

1. Call `pci-setup-msi` — allocates the vector and enables the capability.
2. Spawn the RX pump context — it calls `msi-count` immediately on entry.
3. Enable the device's own interrupt generation (write the interrupt mask
   register) **last**, after both the above are in place.

From `lisp/drivers/rtl8169.clp`:

```scheme
(let ((msi (pci-setup-msi ecam)))
  ;; ... spawn TX context ...
  ;; Spawn RX pump BEFORE enabling IMR.
  (spawn-restricted '() (lambda ()
    (let loop ((seen (msi-count msi)))
      (bytes-u16-set! regs IMR 0)             ; mask while servicing
      (bytes-u16-set! regs ISR-REG           ; ack all causes
                      (bytes-u16-ref regs ISR-REG))
      (rx-sweep! rxring rxbuf handler)
      (bytes-u16-set! regs IMR               ; re-enable (catches pending)
                      (bitwise-or INTR-ROK INTR-TOK))
      (if (> (msi-count msi) seen)
          (loop (msi-count msi))
          (begin (msi-wait msi seen) (loop (msi-count msi)))))))
  ;; Enable device interrupt generation LAST.
  (bytes-u16-set! regs ISR-REG #xFFFF)       ; clear stale causes
  (bytes-u16-set! regs IMR
                  (bitwise-or INTR-ROK INTR-TOK)))
```

### Polling alternative: msi-wait for event-ring polling

Some controllers (xHCI) use an event ring rather than a level-sensitive
interrupt. The driver polls the ring for new entries and uses `msi-wait` purely
as a yield point — the actual completion check is the OWN/Cycle bit in the
ring, not the MSI counter:

```scheme
(let loop ((seen (msi-count msi)))
  (if (event-ring-has-entry?)
      (begin (process-event!) (loop (msi-count msi)))
      (begin (msi-wait msi seen) (loop (msi-count msi)))))
```

### Legacy ISA IRQs (non-PCI)

For ISA-IRQ devices (the PS/2 controller, serial REPL), use `sys-irq`:
`(irq-register gsi)` → handle; `(irq-count handle)` / `(irq-wait handle seen)`
mirror the MSI API exactly. PCI MSI drivers do **not** need `sys-irq`.

---

## 5. Registering with a Core* service

After bring-up, send a registration message to the relevant `Core*` service
handle (passed into your init function by `init.clp`). The service then
dispatches commands back to your driver context by message.

### Network (`corenetwork`) — [`../servers/corenetwork.md`](../servers/corenetwork.md)

```scheme
;; Spawn a TX context that receives (tx frame len [reply]) messages.
(let ((tx-ctx (spawn-restricted '() (lambda ()
                 (let loop ()
                   (let ((m (recv)))
                     (if (eq? (car m) 'tx)
                         (tx-send! regs ring buf free-slot (cadr m) (caddr m)))
                     (loop)))))))
  ;; Register: hand the stack the MAC and the TX context handle.
  (send net (list 'register-nic mac tx-ctx)))

;; RX: forward snapshotted frames from the pump.
(send net (list 'rx (copy-bytes rxbuf off len) len))
```

### Storage (`corestorage`) — [`../servers/corestorage.md`](../servers/corestorage.md)

```scheme
;; Spawn a driver context that answers (read lba count reply) /
;; (write lba count data reply) with (complete status [bytes]).
(let ((driver-ctx (spawn-restricted '() ...)))
  (send storage (list 'register-blockdev 'mydev0 512 sector-count driver-ctx)))
```

### Audio (`coreaudio`) — [`../servers/coreaudio.md`](../servers/coreaudio.md)

```scheme
;; enumerate-endpoints returns the classified endpoint list.
(send audio (list 'register name (self) (endpoint-descs eps)))
```

After registration, `coreaudio` sends `'tone`, `'play`, `'set-volume`,
`'capture-start`, etc. to the card context (`(self)` above).

### Display (`coredisplay`) — [`../servers/coredisplay.md`](../servers/coredisplay.md)

```scheme
;; Register a linear framebuffer driver.
(send display-svc
      (list 'register "My GPU" 'connected (self)
            (list width height pitch 32)))
```

The driver context then answers `'get-framebuffer`, `'get-displayinfo`,
`'flush`, `'fill`, etc.

### USB class drivers (`coreusb`) — [`../servers/coreusb.md`](../servers/coreusb.md)

USB class drivers register *before* the host controller is brought up:

```scheme
(send usb (list 'register-class USB-CLASS-MY-CLASS ctx))
```

`coreusb` calls `ctx` when a matching device connects.

---

## 6. The RX-handler locking rule

!!! warning "Never hold a driver lock across a receive-handler call"
    This rule applies to the network RX path and any service that answers
    requests synchronously. For *why* the Lisp message model dissolves this
    deadlock class, see
    [Message passing & concurrency](../concepts/message-passing.md#driver-rx-handlers-may-re-enter-the-tx-path);
    this section is the driver-side checklist.

In the C driver model, calling `network_rx_packet()` runs the network stack
synchronously. If a received frame requires a reply (ARP who-has, ICMP echo,
UDP echo), the stack calls back into the same driver's TX function — which tries
to acquire the **same lock** the RX handler is still holding. The result is an
immediate self-deadlock; the interrupt mask stays disabled; the RX ring fills;
no further frames are processed. This is the exact bug documented in
`notes/drivers/rtl8168-bringup.md` §"Self-deadlock on reply".

The Lisp driver model avoids this structurally by separating the TX and RX
contexts: the RX pump is a dedicated `spawn-restricted` context that sends
frames to `corenetwork` *asynchronously* (`send` never blocks); the TX context
is a separate loop that receives commands from `corenetwork`. Because message
`send` to a different context is non-blocking, the RX pump never re-enters the
TX path synchronously — the deadlock cannot arise naturally.

If your driver shares state between the RX pump and another loop (for example,
a statistics cell or a shared descriptor index), release any logical lock before
calling `(send net (list 'rx ...))`. The RX pump and TX context should each
own their ring/index exclusively if possible, with state shared only through
the `make-cell`/`cell-ref`/`cell-set!` boxes from `driver-util` (which are
single-word atomic updates, not locks).

For drivers feeding non-network services the same principle applies: if a
service can reply synchronously (e.g. `corestorage` sending a `'complete` back
to the same context), do not hold cross-context state while that call is in
flight.

---

## 7. Binding in `init.clp`

`lisp/init.clp` is the **only place** where drivers are bound to hardware. Edit
it in three places:

### 1. Add your module to the import list

```scheme
(define-module init
  (export system-init start-repl play-tone set-vol)
  (import ... mydev ...)   ; add here
  ...)
```

### 2. Call your init function in `system-init`

For a VID/DID match (single or multi-device):

```scheme
;; Single device:
(let ((ecam (pci-find MY-VID MY-DID)))
  (if ecam (mydev-init svc ecam)))

;; Multiple devices of the same type:
(for-each (lambda (ecam) (mydev-init svc ecam))
          (pci-find-all MY-VID MY-DID))
```

For a class-generic driver (like hdaudio, which matches any HDA controller):

```scheme
(for-each
  (lambda (ecam)
    (mydev-init svc 'mydev0 ecam))
  (pci-find-class-all MY-BASE-CLASS MY-SUBCLASS))
```

### 3. Capability injection (if needed)

If your driver's bring-up context needs a capability that restricted contexts
normally cannot acquire, inject it as a closure. The compositor pattern is the
canonical example: `init` holds `sys-mmio` and passes `dma-alloc-wb`,
`grant-mint`, and `grant-revoke` as captured closures into the compositor init,
so the compositor can allocate WB DMA and mint shared-memory grants without
itself importing `sys-mmio`. The `spawn-restricted` that runs the bring-up loop
lists `'(sys-shm)` explicitly so it can delegate `map-grant` to the client
contexts it spawns.

For ordinary device drivers that only need `sys-mmio` and `sys-pci`, no
injection is necessary — the driver module imports those directly.

### Bring-up in a spawned context

Operations that block (resets, IDENTIFY, codec enumeration) must run in a
spawned context so `wait-until`/`sleep` actually yield the scheduler:

```scheme
(define (mydev-init svc ecam)
  (let ((cfg (mmio-map ecam #x1000)))
    (pci-enable-mem-bus-master! cfg)
    (let ((base (bar-base cfg 0)))
      (if (= base 0)
          (begin (display "[mydev] no BAR0") (newline) #f)
          ;; Fire-and-forget: spawn the bring-up so resets yield.
          (spawn-restricted '()
            (lambda () (mydev-bringup svc cfg base ecam)))))))
```

`init.clp`'s `system-init` does not `recv` the result: the bring-up context
logs, registers, and exits autonomously. This mirrors `ahci-init`,
`virtio-gpu-init`, and `hdaudio-init`.

---

## Next steps

- [Add a Core\* server](add-a-server.md) — design the message protocol your
  driver registers against
- [Debug the OS](debugging.md) — the REPL, the `sys-debug` context inspector,
  and interrupt-delivery triage when your MSI never fires
- [Message passing & concurrency](../concepts/message-passing.md) — the model
  behind `spawn-restricted`, `send`/`recv`, and the RX-handler rule

## See also

- [`../drivers/rtl8169.md`](../drivers/rtl8169.md) — the reference NIC driver
  (descriptor rings, MSI, BAR self-assignment, D3→D0 transition)
- [`../drivers/virtio-net.md`](../drivers/virtio-net.md) — virtio transport
  (capability walk, virtqueue setup, MSI-X via `pci-setup-msi`)
- [`../drivers/ahci.md`](../drivers/ahci.md) — storage driver (DMA-32,
  MSI-last ordering, bring-up in a spawned context)
- [`../drivers/hdaudio.md`](../drivers/hdaudio.md) — class-matched driver;
  `pci-find-class-all`; `dma-alloc-32` for CORB/RIRB
- [`../servers/corenetwork.md`](../servers/corenetwork.md) — network stack
  message protocol (`register-nic`, `rx`, RDT, DHCP)
- [`../servers/corestorage.md`](../servers/corestorage.md) — storage registry
  (`register-blockdev`, read/write protocol)
- [`../servers/coreaudio.md`](../servers/coreaudio.md) — audio service
  (card registration, endpoint model, tone/capture)
- [`../servers/coredisplay.md`](../servers/coredisplay.md) — display registry
- [Lisp VM reference](../vm/index.md) — full Lisp VM primitive reference
