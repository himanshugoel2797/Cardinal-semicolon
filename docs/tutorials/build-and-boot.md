# Build & boot Cardinal; in QEMU

*Take a fresh clone from zero to a running OS with a live Lisp REPL on the serial console.*

This tutorial walks you through every step: installing the toolchain, building the kernel and
its Lisp-level OS services, producing a bootable ISO, and booting it headlessly in QEMU so
you can watch the system come up and — optionally — drop into the interactive Lisp REPL.

---

## Prerequisites

### System packages (not in the conda env)

The repo-local conda environment (`.devenv/`) supplies the entire
cross-compiler toolchain (Clang 20, lld, cmake, ninja). It does **not** supply
the ISO-building and emulation tools, which must come from your system package
manager:

```bash
sudo apt install qemu-system-x86 grub-pc-bin grub-efi-amd64-bin mtools xorriso
```

These are required to produce `build/ISO/os.iso` and to boot it. If you only
want to compile the kernel and modules (no ISO, no QEMU), you can skip this
step.

### Conda

`scripts/devenv/setup.sh` manages the toolchain env and requires `conda` on
your `PATH`. Install [Miniforge](https://github.com/conda-forge/miniforge) or
Miniconda if you do not have it already; the setup script errors out with a
clear message if `conda` is absent.

---

## Step 1 — One-time toolchain bootstrap

From the repo root, run the setup script once. It creates (or updates in place)
the conda env at `.devenv/`, pinned to the versions in
`scripts/devenv/environment.yml`:

```bash
./scripts/devenv/setup.sh
```

When it finishes you will see something like:

```
[setup] clang: clang version 20.x.x
[setup] lld:   LLD 20.x.x
[setup] cmake: cmake version 3.x.x
[setup] Start a build session with:  source ./scripts/devenv/activate.sh
```

Re-running is safe and incremental — it updates the env in place if the
`environment.yml` pins change.

---

## Step 2 — Activate the build environment (every session)

The conda env must be **sourced** (not executed) so its `PATH` changes persist
in your shell:

```bash
source ./scripts/devenv/activate.sh
```

You should see a confirmation line. Every subsequent `clang`, `cmake`, and
`ninja` invocation in that shell now uses the repo-local toolchain.

!!! tip "Shell aliases"
    Add `alias cardinal-env='source /path/to/repo/scripts/devenv/activate.sh'`
    to your shell profile so you do not have to remember the path.

---

## Step 3 — Build

Run the top-level build script:

```bash
./scripts/build.sh
```

This performs **two separate CMake builds**:

| Build | Source | Output | Compiler |
|---|---|---|---|
| Host tools | `utils/` | `utils_build/` | Your host `cc` |
| Kernel + modules | repo root | `build/` | `clang --target=x86_64-elf` |

**Why two builds?** `utils/sign_exec` wraps each compiled module in a signed
`ModuleHeader` (`.celf`). It runs on your build machine, so it must be compiled
with the host compiler. The kernel verifies each `.celf` at load time — this is
the security boundary. Everything in `build/` is cross-compiled freestanding for
the `x86_64-elf` target; no libc, no red zone, all SSE disabled.

When the build succeeds you will find:

- `build/kernel/kernel.bin` — the microkernel
- `build/*.celf` / `build/ISO/isodir/boot/` — signed modules (~28 of them),
  including `SysLisp.celf`, which carries the kernel-resident bytecode VM that
  runs everything above the `Sys*` core
- `utils_build/sign_exec/sign_exec` — the signing tool

!!! note "Overrides"
    `BUILD_TYPE=Release ./scripts/build.sh` builds with `-O3` (verified to
    boot). `GEN="Unix Makefiles"` switches from Ninja. Both are
    environment-variable overrides — no CMake flags needed.

!!! warning "Incremental rebuilds after adding source files"
    CMake globs source files at configure time. If you add a new `.c` file
    inside a module's `src/` directory, re-run `cmake -S . -B build` (just the
    configure step) before the next `cmake --build build`, otherwise Ninja will
    not pick it up.

---

## Step 4 — Build the bootable ISO

```bash
cmake --build build --target image
```

This runs the `image` custom target defined in
`platform/x86_64/pc/flags.cmake`. It:

1. Tars the signed `.celf` modules, boot scripts (`loadscript.txt`,
   `apscript.txt`), and the Lisp source tree (`lisp/`) into a single initrd
   at `build/ISO/isodir/boot/initrd`.
2. Copies `kernel.bin` alongside it.
3. Calls `grub-mkstandalone` to build a self-contained UEFI image
   (`EFI/BOOT/BOOTX64.EFI`) and `grub-mkrescue` to produce the final hybrid
   ISO at **`build/ISO/os.iso`**.

!!! warning "Always run the full build first"
    The `image` target depends on all `.celf` custom targets, but if you run it
    in isolation after a clean checkout it may tar an incomplete initrd and the
    kernel will panic at boot with "Failed to find module". Always run
    `./scripts/build.sh` at least once before invoking `--target image`.

!!! warning "Boot script line endings"
    `loadscript.txt` and `apscript.txt` (the kernel's bring-up sequences) use
    **CRLF** line endings. The kernel boot-script parser requires this; if you
    ever edit these files, preserve CRLF or the parser will panic with "Unknown
    Command".

---

## Step 5 — Boot in QEMU

```bash
./scripts/run-qemu.sh
```

This boots `build/ISO/os.iso` headlessly: the kernel's COM1 debug output
(115200 8N1) is forwarded to your terminal via `-serial stdio`. There is no
graphical window unless you set `DISPLAY_MODE=gtk` or `DISPLAY_MODE=sdl`.

### Key environment knobs

| Variable | Default | Notes |
|---|---|---|
| `ISO` | `build/ISO/os.iso` | Path to the ISO to boot |
| `MACHINE` | `q35` | Do not change. The q35 machine exposes an ACPI MCFG table; the PCI registration code in the Lisp drivers requires it. The legacy `i440fx` / `pc` machine has no MCFG. |
| `ACCEL` | `auto` | Automatically uses KVM if `/dev/kvm` is accessible, falls back to TCG. Pass `kvm` or `tcg` to force. |
| `MEM` | `512` | Guest RAM in MiB |
| `SMP` | `2` | Guest CPU count (BSP + APs; each core runs its own Lisp scheduler loop) |
| `TIMEOUT` | `30` | Seconds before QEMU auto-exits. Set to `0` to disable. |
| `GPU` | `none` | Add `virtio` or `virtio-vga` to attach a display adapter |
| `NIC` | `none` | Add `virtio-net` or `rtl8139` to attach a NIC on slirp |

Example — boot with KVM and a longer timeout:

```bash
ACCEL=kvm TIMEOUT=120 ./scripts/run-qemu.sh
```

### KVM vs TCG

KVM is strongly recommended for interactive use: the OS boots in seconds under
KVM versus tens of seconds under TCG software emulation. If `auto` falls back
to TCG and you have the hardware, add yourself to the `kvm` group and re-login:

```bash
sudo usermod -aG kvm $USER
# then log out and back in
```

---

## Step 6 — Reading the boot output

A successful boot produces a log on your terminal that looks roughly like this
(abbreviated):

```
[SysLisp] interpreter-as-scheduler, multi-core bring-up
[SysLisp] <N> passed, 0 failed
[SysLisp] ALL TESTS PASSED
[SysLisp] system heap frozen; releasing APs as Lisp cores
[SysLisp] core 0 online: lisp scheduler running, proof -> 2000
[SysLisp] core 1 online: lisp scheduler running, proof -> 2000
```

What each line means:

- **`ALL TESTS PASSED`** — The single-core self-test suite inside `SysLisp`
  ran clean before going multi-core. If you see `SELF-TEST FAILED` instead,
  the VM itself has a bug; nothing above the `Sys*` core will be reliable.
- **`system heap frozen; releasing APs as Lisp cores`** — The shared system
  heap is frozen (the conservative GC cannot see other cores' stacks), and
  the application processors are released. Each AP joins as another Lisp
  scheduler core.
- **`core N online`** — Each CPU (BSP = core 0, then one line per AP) confirms
  it is running the Lisp scheduler. The `proof -> 2000` is a small heap-
  allocating computation that validates the per-core GC is functional.

After the cores are online, `system-init` in `lisp/init.clp` runs the full OS
bring-up: it `pci-find`s hardware and brings up services — input, audio, display,
network, storage, USB — each gated on whether the device is present. With the
default headless QEMU invocation (no GPU, no NIC), most drivers simply log "no
device" and move on. That is expected.

---

## Optional — the interactive serial REPL

The interactive Lisp REPL is opt-in. It requires a separate ISO (built from the
same binaries with a different GRUB config that passes `cardinal.repl` on the
kernel command line).

Build the REPL ISO:

```bash
cmake --build build --target repl-image
```

This produces `build/ISO/os-repl.iso`. Launch the raw serial terminal:

```bash
python3 scripts/serial-repl.py
```

`serial-repl.py` launches QEMU internally (booting `build/ISO/os-repl.iso` by
default) and relays COM1 bytes raw — the boot log streams to your terminal
until the REPL starts, then your input goes to the REPL. When the OS prints:

```
[repl] serial REPL ready on COM1 -- try (play-tone)
```

you are at the REPL. Type any Lisp expression and press Enter:

```scheme
(+ 1 2)
; => 3
(uptime-ns)
; => <nanoseconds since boot>
```

The REPL context has full root authority and imports `play-tone` and `set-vol`
from `init` for interactive audio testing. The OS continues scheduling all other
contexts normally while you type — the REPL parks on COM1 RX (ISA IRQ 4) and
only runs when a byte arrives.

Once the REPL starts, component logs no longer stream to serial: they go to the
in-memory per-source log store and are read back with `(log-sources)`,
`(log-dump "src")`, and `(log-tail "src" n)`. See
[Your first REPL session](first-repl-session.md) for a worked example.

---

## Troubleshooting

**`error: sign_exec was not produced`**
: The host-tools build failed. Check that your host compiler (`cc`) is present
  and working. The host build lives in `utils_build/` and is entirely separate
  from the cross build.

**`error: ISO not found: build/ISO/os.iso`**
: You have not run `cmake --build build --target image` yet, or the build
  failed. The ISO is not produced by `./scripts/build.sh` alone.

**`/dev/kvm not accessible`**
: Add yourself to the `kvm` group (`sudo usermod -aG kvm $USER`) and re-login.
  The script falls back to TCG automatically — the OS will still boot, just
  more slowly.

**Kernel panics with "Failed to find module"**
: The initrd is incomplete. This happens when `--target image` runs without
  all `.celf` targets having staged their outputs. Run the full
  `./scripts/build.sh` first, then rebuild the image.

**"Unknown Command" during boot**
: `loadscript.txt` or `apscript.txt` has been saved with LF-only line endings.
  The boot-script parser requires CRLF. Restore with
  `unix2dos loadscript.txt apscript.txt` (or equivalent).

---

## Next steps

- [Your first REPL session](first-repl-session.md) — explore the live OS from
  the serial Lisp prompt: inspect running contexts, send messages between
  services, and try the built-in debug primitives.
- [System overview](../concepts/system-overview.md) — understand the layered
  architecture: the microkernel, the `Sys*` C modules, and the Lisp VM that
  runs everything above them.
