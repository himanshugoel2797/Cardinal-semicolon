# lfb

> Linear framebuffer display driver — maps the firmware-provided boot GOP/VESA framebuffer and registers it as a fallback display with [coredisplay](../servers/coredisplay.md).

| | |
|---|---|
| **Source** | `lisp/drivers/lfb.clp` |
| **Kind** | driver |
| **Bound by** | `lisp/init.clp` — always attempted; gated on `(not (pci-find #x1af4 #x1050))` (i.e. only when no virtio-gpu device is present) |
| **Registers with** | [coredisplay](../servers/coredisplay.md) via `display-register` (internally via `lfb-register-msg`) |
| **Capabilities** | `sys-reg` (read boot-info registry keys), `sys-mmio` (map framebuffer MMIO), `driver-util` |

## Overview

The boot firmware (multiboot2/GRUB) configures a linear framebuffer before handing control to the kernel. `SysReg` records its physical address and geometry under `HW/BOOTINFO/FRAMEBUFFER`. `lfb` reads those keys, maps the framebuffer as MMIO, and registers a `"Linear Framebuffer"` display entry with coredisplay. It acts as the narrowest-capability display driver in the tree: no PCI device, no DMA, no IRQ. The framebuffer is directly scanned out by the display controller hardware (e.g. QEMU `std-vga`, BIOS/UEFI GOP), so `flush` is a deliberate no-op.

`lfb` is the fallback path. `init.clp` tries `virtio-gpu` first; `lfb` is only started when `pci-find` finds no virtio-gpu device on the bus. This mirrors the old C policy: "load lfb only if no display has already registered."

The driver loop runs in a `spawn-restricted` context (no extra capabilities beyond those it imports) rather than in the root `init` context. This matters because the bring-up test-pattern paint involves millions of MMIO writes; only a spawned context has a per-context GC'd heap — running the paint in the root init context would exhaust the system heap.

## Initialization

`init.clp` calls:

```scheme
(lfb-init display-svc paint?)
```

- `display-svc` — handle to the coredisplay service (returned by `start-display-service`).
- `paint?` — boolean; `#t` causes the driver to paint a bring-up gradient before registering. `init.clp` passes `(not demo?)`: when `cardinal.gfxdemo` is on the kernel command line the paint is skipped so the driver registers immediately and the graphics demo is not blocked waiting for millions of slow uncached-MMIO writes.

`lfb-init` reads framebuffer geometry from `SysReg` via `fb-params`. If any of the four required keys (`PHYS_ADDR`, `PITCH`, `WIDTH`, `HEIGHT`) is absent or zero, it logs `[lfb] no boot framebuffer; not registering` and returns `#f`. Otherwise it maps the framebuffer and spawns `lfb-driver-loop` as a restricted context, returning that context's handle.

### Registry keys read (`HW/BOOTINFO/FRAMEBUFFER`)

| Key | Required | Default when absent | Description |
|-----|----------|--------------------|-|
| `PHYS_ADDR` | yes | — | Physical base address of the framebuffer |
| `PITCH` | yes | — | Row stride in bytes |
| `WIDTH` | yes | — | Width in pixels |
| `HEIGHT` | yes | — | Height in pixels |
| `RED_OFFSET` | no | `16` | Bit offset of the red channel in a pixel word |
| `GREEN_OFFSET` | no | `8` | Bit offset of the green channel |
| `BLUE_OFFSET` | no | `0` | Bit offset of the blue channel |

There is no `BPP` registry key (the C `bootinfo.c` writer only records masks/offsets). Bits-per-pixel is inferred as `(* 8 pitch) / width`; if the result is zero, 32 is used. `BYTES-PER-PIXEL` is the constant `4`.

## Message protocol

Consumers obtain the driver's context handle from coredisplay and `send` it directly. The driver loop (`lfb-driver-loop`) handles the following tags.

### `get-framebuffer`

- **Request:** `(get-framebuffer reply-ctx)` — `reply-ctx` is the context to which the reply is sent.
- **Reply:** `(fb width height pitch)` — `fb` is the MMIO-mapped bytes object, `width`/`height` in pixels, `pitch` in bytes.
- **Notes:** The returned `fb` object is live MMIO. Callers that write to it write directly to the scanned-out framebuffer. No copy is made; there is no double-buffer in this driver.

### `get-displayinfo`

- **Request:** `(get-displayinfo reply-ctx)`
- **Reply:** `(width height pitch bpp refresh)` — `bpp` is always `32`; `refresh` is always `60` (nominal; the linear framebuffer has no vsync mechanism).

### `get-status`

- **Request:** `(get-status reply-ctx)`
- **Reply:** `'connected` — always; the linear framebuffer is always considered present once the driver is running.

### `flush`

- **Request:** `(flush)` — no additional fields.
- **Reply:** none (no-op, returns `'noop` internally).
- **Rationale:** The linear framebuffer is directly scanned out by the display controller. There is nothing to push to a host, no virtqueue, and no DMA descriptor ring. Writes to `fb` appear on screen immediately.

### `fill`

- **Request:** `(fill color)` — `color` is a 32-bit packed pixel word (use `pack-rgb` to construct it).
- **Reply:** none.
- **Effect:** Fills every pixel row with `color` using `bytes-fill32!` (a bulk vectorized C fill), one row at a time. Padding bytes between rows (when `pitch > width * 4`) are left untouched.

## Exported functions

### `(lfb-init display-svc paint?)`

Entry point. Reads boot framebuffer geometry, maps MMIO, and spawns the driver loop. Returns the spawned context handle, or `#f` when no usable boot framebuffer is found.

### `(pack-rgb r g b r-off g-off b-off)`

Pure function. Assembles an R8G8B8 colour into a 32-bit pixel word given per-channel bit offsets. For the standard `X8R8G8B8` layout (`r-off=16 g-off=8 b-off=0`) this produces the familiar `0x00RRGGBB`. Exported for unit testing and for callers that construct pixel values without knowing the hardware channel layout.

```scheme
(pack-rgb 255 0 128 16 8 0)   ; => #x00FF0080
```

### `(lfb-register-msg display-svc width height pitch ctx)`

Sends the `register` message to `display-svc`. Factored out so the self-test can drive it without live hardware. `coredisplay` stores the registration payload verbatim; `ctx` is the driver loop's own handle (`(self)`) so coredisplay can forward client requests to it.

```scheme
(send display-svc
      (list 'register "Linear Framebuffer" 'unknown ctx
            (list width height pitch 32)))
```

Connection type is reported as `'unknown` (the linear framebuffer has no connector model).

## Notes / gotchas

**No double-buffering, no WC mapping.** The framebuffer is mapped as plain MMIO (uncached) via `mmio-map`. There is no write-combining (WC) PAT mapping and no shadow buffer in this driver. Callers wanting WC or double-buffer behaviour must implement it themselves (the graphics demo layer above `lfb` does exactly this — see `notes/AUDIT.md` and the `cardinal.gfxdemo` path in `init.clp`).

**Bring-up test pattern.** On start the driver paints a smooth gradient (`pixel(x,y) = pack-rgb (x mod 256) (y mod 256) 128`) as a live proof that the MMIO mapping reaches the scanned-out pixels. This runs in the spawned context, not in `lfb-init`'s caller, to avoid exhausting the root init heap. The pattern is skipped (`paint? = #f`) when `cardinal.gfxdemo` is active.

**Fallback driver only.** `lfb` is not started when `virtio-gpu` (PCI `1af4:1050`) is present. Extending to additional GPU drivers requires adding their `pci-find` results to the gate condition in `init.clp`.

**Framebuffer size.** The MMIO region mapped is `pitch * height` bytes — exactly the C port's `vmem_phystovirt` size. Padding pixels in rows wider than `width * 4` bytes are mapped but the driver never writes them (fill uses `bytes-fill32!` per row up to `width` pixels only).

**`flush` is a deliberate no-op.** Code that calls `flush` to push a composed frame should verify at the coredisplay level which driver is active; on `virtio-gpu` the flush performs real virtqueue work, but on `lfb` it returns immediately without any action.

**Per-pixel loops are slow.** The MMIO framebuffer is uncached. The `fb-test-pattern!` path uses an incremental offset to minimize per-pixel overhead, but it is still millions of interpreted iterations. `fb-fill!` avoids this via `bytes-fill32!`. Any new per-pixel path added above `lfb` should use bulk primitives (`bytes-fill32!`, `bytes-copy!`) rather than per-pixel `bytes-u32-set!` loops.
