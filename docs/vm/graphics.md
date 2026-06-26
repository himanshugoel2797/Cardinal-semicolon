# Graphics & driver libraries

*Part of the [Lisp VM Reference](index.md).*

> The Lisp-level libraries in `lisp/lib/`: the `graphics` / `font` / `ttf`
> 2D rendering stack and the shared `driver-util` helpers.

## 2D Graphics and font library

These are Lisp-level modules in `lisp/lib/`.  They require no system
capabilities (only `bytes` and the ambient `gfx-*!` C primitives) and are
fully host-testable.

### Low-level 2D primitives (C, ambient)

These are available without import — they are registered in the default
environment by `lisp_install_primitives`.

| Signature | Description |
|-----------|-------------|
| `(gfx-fill-rect! dst stride dw dh x y w h color)` | Fill a clipped rectangle in a 32-bit-per-pixel `bytes` |
| `(gfx-blit! dst dstride dw dh dx dy src sstride sw sh)` | Opaque blit of a `src` image to `(dx,dy)` in `dst`, clipped |
| `(gfx-blend! dst dstride dw dh dx dy src sstride sw sh)` | Alpha-composite an ARGB `src` (alpha in top byte) over `dst`, clipped |
| `(gfx-glyph! dst stride dw dh dx dy bitmap boff gw gh fg bg draw-bg scale)` | Blit a 1-bpp glyph (MSB-left, `ceil(gw/8)` bytes per row) scaled `scale×scale` |
| `(gfx-cover! dst stride dw dh dx dy cover cstride cw ch fg)` | Composite solid color `fg` through an 8-bit coverage mask (antialiased text path) |

All `gfx-*!` primitives clip to `(0,0,dw,dh)` (off-screen draws are safe),
require 4-aligned data and stride for 32-bit stores, and are NON-volatile (not
for MMIO registers).

### `graphics` module — surface-based 2D drawing

```scheme
(import graphics)
```

#### Surface construction

```scheme
; Software surface over a plain 0x00RRGGBB framebuffer:
(make-surface fb width height stride)

; Full control over channel offsets and HW backend:
(make-surface* fb width height stride r-off g-off b-off backend)
```

Accessors: `surface-fb`, `surface-width`, `surface-height`,
`surface-stride`, `surface-r-off`, `surface-g-off`, `surface-b-off`,
`surface-backend`.

The `backend` is an alist of `(op-symbol . proc)` hardware acceleration
overrides.  Every drawing operation checks the backend first; the software
fallback (a `gfx-*!` call) runs if the op is absent.

#### Colour packing

```scheme
(rgb  surf r g b)       ; => packed pixel in surface's channel layout
(argb surf a r g b)     ; => packed pixel with 8-bit alpha in top byte
```

#### Drawing operations

| Signature | Description |
|-----------|-------------|
| `(clear surf color)` | Fill the entire surface |
| `(fill-rect surf x y w h color)` | Filled rectangle (hardware-acceleratable) |
| `(draw-rect surf x y w h t color)` | Rectangle outline of thickness `t` |
| `(draw-hline surf x y w color)` | Horizontal line |
| `(draw-vline surf x y h color)` | Vertical line |
| `(draw-line surf x0 y0 x1 y1 color)` | Bresenham line |
| `(draw-circle surf cx cy r color)` | Midpoint circle outline |
| `(fill-circle surf cx cy r color)` | Filled circle |
| `(put-pixel surf x y color)` | Write one pixel (bounds-checked) |
| `(get-pixel surf x y)` | Read one pixel; `0` if out of bounds |
| `(blit dst src dx dy)` | Opaque blit of `src` surface to `(dx,dy)` in `dst` |
| `(blit-alpha dst src dx dy)` | Alpha-composite `src` (ARGB) over `dst` |
| `(draw-glyph surf x y bitmap boff gw gh fg bg draw-bg? scale)` | Bitmap glyph blit |

#### Double buffering

```scheme
(make-double-buffer front-surface)   ; => double-buffer pair (back . front)
(db-back db)                         ; the cached (WB) compositing surface
(db-front db)                        ; the scanout (WC) surface
(db-flush db)                        ; bulk-copy entire back -> front + sfence
(db-flush-rect db x y w h)          ; copy only the dirty region
```

#### Damage tracking

```scheme
(make-damage)              ; mutable damage list
(damage-add! d x y w h)   ; record a dirtied rectangle
(damage-rects d)           ; list of (x y w h) rects
(damage-clear! d)          ; reset for next frame
(damage-empty? d)          ; #t if no damage recorded
(db-flush-damage db d)     ; flush only damaged rects, then clear the list
```

### `font` module — bitmap font text rendering

```scheme
(import font)
```

Renders fixed-cell bitmap fonts (1-bpp, MSB-left row encoding) via
`draw-glyph`.  The included 8×16 VGA-style font covers ASCII 32–126.

```scheme
(define FONT8X16-PATH "./lisp/data/font8x16.bin")  ; initrd path
(define FONT8X16-W 8)
(define FONT8X16-H 16)

; Construction (the caller supplies bytes from sys-initrd):
(make-font bitmap glyph-w glyph-h)

; Accessors:
(font-glyph-w f)    (font-glyph-h f)    (font-cellbytes f)
```

| Signature | Description |
|-----------|-------------|
| `(draw-char surf font x y ch fg bg draw-bg? scale)` | Draw one character |
| `(draw-text surf font x y str fg bg draw-bg? scale)` | Draw a string; newlines wrap; returns next y |
| `(text-width font str scale)` | Pixel width of the widest line |

### `ttf` module — TrueType antialiased text

```scheme
(import ttf)   ; requires sys-ttf and graphics in the grant
```

Wraps the `ttf-rasterize` kernel primitive with a per-font glyph cache and
vertical-metrics cache (both `equal?`-keyed hash tables).  Uses `gfx-cover!`
to composite glyphs as antialiased coverage bitmaps.

```scheme
(define TTF-FONT-PATH "./lisp/data/DejaVuSans-subset.ttf")

; Construction (caller supplies font bytes from sys-initrd):
(make-ttf-font bytes)
```

| Signature | Description |
|-----------|-------------|
| `(ttf-draw-text surf font x y str color px)` | Draw string antialiased at pixel size `px`; newlines wrap; returns baseline y of last line |
| `(ttf-text-width font str px)` | Pixel width of widest line (sum of cached glyph advances) |
| `(ttf-line-height font px)` | Line pitch in pixels (`ascent - descent + linegap`) |
| `(ttf-ascent font px)` | Distance from top-of-line to baseline |

Pen model: `(x, y)` is the top-left of the text block; the baseline sits
`ascent` pixels below `y`.  Each glyph's coverage box is placed at
`(x + xoff, baseline + yoff)` (yoff is negative — the box top is above the
baseline).

---

## `driver-util` module — shared driver utilities

```scheme
(import driver-util)
```

A general-purpose library imported by most drivers and servers.

### List helpers

| Signature | Description |
|-----------|-------------|
| `(nth lst k)` | Zero-based list index (like `list-ref`) |

### Mutable cell

```scheme
(make-cell v)    ; make a mutable 1-element box holding v
(cell-ref c)     ; read current value
(cell-set! c v)  ; write new value
```

### Big-endian byte access

Network protocol headers are big-endian; these operate on a `bytes` buffer:

| Signature | Description |
|-----------|-------------|
| `(put-be16! b off v)` | Write 16-bit big-endian |
| `(get-be16 b off)` | Read 16-bit big-endian |
| `(put-be32! b off v)` | Write 32-bit big-endian |
| `(get-be32 b off)` | Read 32-bit big-endian |

### Buffer utilities

| Signature | Description |
|-----------|-------------|
| `(copy-bytes src off len)` | Copy `len` bytes from `src[off..)` into a fresh owned buffer |
| `(bytes-copy-into! dst off src len)` | Copy `len` bytes from `src[0..)` into `dst[off..)` |
| `(put-list! b off lst)` | Write a list of byte values into `b` starting at `off` |

### PCI helpers

| Signature | Description |
|-----------|-------------|
| `PCI-COMMAND` | Constant `#x04` (PCI command register offset) |
| `(bar-base cfg bar-idx)` | Decode a BAR's physical base address (handles 64-bit BARs) |
| `(pci-enable-mem-bus-master! cfg)` | Set COMMAND bits 1 (mem-space) and 2 (bus-master) |

### Polling

```scheme
; Poll pred with a spin budget before falling back to sleep-based polling:
(wait-until-spin pred timeout-ns spin-ns)

; Poll without a spin budget (for ms-scale waits):
(wait-until pred timeout-ns)
```

Returns `#t` when `pred` became true, `#f` on timeout.

### Server pattern

```scheme
(serve init step)
```

Spawns a zero-capability restricted context running:

```scheme
(let loop ((state init))
  (loop (step state (recv))))
```

Returns the context handle.

```scheme
(reply-to target msg)
```

Safely sends `msg` to `target` only if `(ctx? target)`; returns `#t` on
success.  Use this instead of bare `send` for reply addresses that arrived in
a client message.
