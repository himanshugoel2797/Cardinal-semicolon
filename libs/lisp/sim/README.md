# Cardinal; Lisp simulator

Run the OS's Lisp **servers** and a **user app** on the host, against **fake,
host-backed drivers**, so application code can be developed and tested without
booting a VM. A real window is the simulated screen; the host keyboard/mouse is
the simulated input device.

This is the interactive sibling of the `libs/lisp/test/` harnesses: those are
one-shot, headless assertions over the Lisp core; this is a live runtime that
pumps a window and an event loop.

```
   host keyboard / mouse                              host window (X11)
            │                                                 ▲
            ▼                                                 │ host-present!
   ┌─────────────────┐   (event …)    ┌──────────────┐   ┌──────────────┐
   │  fake-input.clp │ ─────────────▶ │  coreinput   │   │ fake-display │
   │ (host-input-    │                │  (REAL svc)  │   │   .clp       │
   │   poll)         │                └──────┬───────┘   └──────▲───────┘
   └─────────────────┘                       │ (input …)        │ (present <frame>)
                                             ▼                  │
                                      ┌──────────────────────────┐
                                      │      demo-app.clp         │
                                      │  (subscribe, draw, react) │
                                      └──────────────────────────┘
            coredisplay (REAL registry) ◀── fake-display registers
```

The **servers are real** and unmodified (`lisp/servers/coreinput.clp`,
`lisp/servers/coredisplay.clp`). Only the **drivers are faked**: instead of
`sys-mmio`/`sys-irq`/`sys-pci`, the fakes use three host primitives
(`host-present!`, `host-input-poll`, `host-screen-size`) implemented in
`host_prims.c` behind a pluggable backend. This is the same capability-injection
seam the in-OS code already uses — `lisp/init.clp` is the device binder on the
target; `sim/boot.clp` is its host analogue.

## Build & run

The simulator is pure host code — build it with the **system** clang (it sees
the system X11 headers; the conda cross-toolchain does not):

```bash
CC=/usr/bin/clang libs/lisp/sim/build.sh        # -> libs/lisp/sim/sim
```

Run interactively on a machine with a display:

```bash
libs/lisp/sim/sim          # auto: X11 window if $DISPLAY, else offscreen
```

A window opens; arrow keys move the box, **space** cycles its colour, a mouse
click recentres it. Close the window to stop.

Run headless / in CI (no display): the offscreen backend writes the framebuffer
to a PPM and reads input from a script.

```bash
SIM_BACKEND=offscreen SIM_SCRIPT=libs/lisp/sim/demo-script.txt \
  SIM_PPM=frame.ppm libs/lisp/sim/sim

libs/lisp/sim/selftest.sh  # build + run scripted + assert the pixels (CI)
```

## Backends

| Backend | Selected when | Screen | Input |
|---------|---------------|--------|-------|
| `x11` | `$DISPLAY` set (or `SIM_BACKEND=x11`) and X11 headers were present at build | live window (`XPutImage`) | real keyboard/mouse → PS/2 set-1 scancodes |
| `offscreen` | no display, or `SIM_BACKEND=offscreen` | `SIM_PPM` file, overwritten each present | `SIM_SCRIPT` file |

X11 requested but unavailable (no `$DISPLAY`) falls back to offscreen
automatically, so the same invocation works on a laptop and a build server.

## Environment knobs

| Var | Default | Meaning |
|-----|---------|---------|
| `SIM_BACKEND` | auto | `x11` or `offscreen` |
| `SIM_W` / `SIM_H` | 640 / 480 | screen size |
| `SIM_PPM` | `sim-frame.ppm` | offscreen output image (latest frame) |
| `SIM_PPM_ALL` | unset | also dump numbered `sim-NNNN.ppm` per present |
| `SIM_SCRIPT` | unset | offscreen input script (below) |
| `SIM_PASSES` | 32 | scheduler passes per run-loop iteration |
| `SIM_FRAME_US` | 16000 | sleep between iterations (µs) |
| `SIM_DIR` / `SIM_LISP_DIR` | baked at build | override the `sim/` and `lisp/` source dirs |

### Input script format (offscreen)

One event per line; one event is delivered per poll, so the app repaints between
them. Scancodes are PS/2 set-1 (see `keymap.c`).

```
key 0x4D 1      # Right arrow press   (scancode, pressed 1|0)
key 0x4D 0      # Right arrow release
char a 1        # the 'a' key, by character → its scancode
pointer 500 300 1   # left button down at (500,300)
wait            # a beat: deliver nothing this poll (let the app catch up)
quit            # stop
# comments and blank lines are ignored
```

## Why the app *sends* its frame

IPC in this VM is shared-nothing — `send` deep-copies the message. The host has
no shared physical memory to map, so an app cannot share a mapped scanout with
the display driver the way the in-OS path does (grants / `sys-shm`). Instead the
app composes into its **own** surface and sends the finished frame to
`fake-display`, which presents it — a framebuffer-protocol model, like a remote
display. That keeps the harness honest about isolation at the cost of a
per-present frame copy (fine for event-driven app testing). Zero-copy compositor
client paths (grants) remain in-OS, exactly as `test_compositor.c` notes.

## Files

| File | Role |
|------|------|
| `sim_main.c` | embedding + run loop (pump backend → step scheduler) |
| `host_prims.c` | the `host-*` Lisp primitives + the input queue |
| `backend.h` / `backend_x11.c` / `backend_offscreen.c` | the presentation backends |
| `keymap.c` | X11 KeySym → PS/2 set-1 scancode |
| `boot.clp` | host bring-up policy (the `init.clp` analogue) |
| `fake-display.clp` / `fake-input.clp` | the host-backed fake drivers |
| `demo-app.clp` | a minimal user app (draws + reacts to input) |

## Extending

- **New app:** add `myapp.clp` exporting a `run-myapp` entry, and call it from
  `boot.clp` in place of (or alongside) `run-demo-app`. Its **input** path is
  target-faithful — it subscribes to the real `coreinput` and decodes the same
  scancode tuples as on hardware. Its **output** path uses the sim's
  framebuffer-push verb (`(present <frame>)` to `fake-display`), not the target
  driver verbs (`get-framebuffer`/`flush`), for the shared-nothing reason in
  §"Why the app sends its frame".
- **Run another real server:** `import` it in `boot.clp` and start it — the
  loader already searches `lisp/servers`, `lisp/lib`, `lisp/drivers`. A server
  that needs a `sys-*` capability the host lacks needs a fake driver for that
  capability first (the pattern `fake-display`/`fake-input` follow).
