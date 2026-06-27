# CLAUDE.md

Guidance for AI assistants (and humans) working in this repository.

## What this is

**Cardinal;** is an extremely modular, security-oriented **microkernel
operating system** for `x86_64`, a cleanup and partial rewrite of the older
Cardinal OS. The kernel itself does almost nothing: it loads, verifies, and
relocates ELF modules, then everything else — physical/virtual memory, the
scheduler, interrupts, drivers, and OS services — is a separately-signed
loadable module. See `notes/core/` for the design philosophy and
`README.md` for device-support status.

Only one target is currently wired up: **`x86_64` / `pc`** (BIOS/UEFI PC via
multiboot2 + GRUB).

## Toolchain & build

The toolchain is **Clang/LLVM** targeting freestanding `x86_64-elf`. Clang is
natively a cross compiler, so there is **no separate GCC/binutils cross
build**. `lld` links; `llvm-ar`/`llvm-objcopy`/`llvm-nm`/`llvm-ranlib` replace
binutils. A repo-local conda env under `./.devenv/` (git-ignored) pins
clang/lld/llvm-tools 20, cmake, ninja, and astyle.

```bash
# One-time: build the local toolchain env (idempotent)
./scripts/devenv/setup.sh

# Each build session: put the toolchain on PATH
source ./scripts/devenv/activate.sh

# Build everything: host sign_exec tool + kernel + modules/servers/drivers
./scripts/build.sh
```

`scripts/build.sh` runs **two** CMake builds, and you must keep them separate:

1. **Host tools** — `utils/` → `utils_build/` with the *host* compiler. This
   builds `sign_exec`, which signs modules. It runs on the build machine.
2. **Target** — the repo root → `build/` with
   `clang --target=x86_64-elf` (driven via
   `-DCMAKE_SYSTEM_NAME=Generic -DCMAKE_C_COMPILER=clang -DCMAKE_ASM_COMPILER=clang`).

Override `BUILD_TYPE` (default `Debug`) or `GEN` (default `Ninja`) via env vars.

Artifacts land in `build/`: `kernel/kernel.bin` and the signed `*.celf`
modules, also staged under `build/ISO/isodir/boot/`.

### Bootable ISO & running

ISO/QEMU steps need system packages **not** in the conda env
(`qemu-system-x86`, `grub-pc-bin`, `grub-efi-amd64-bin`, `mtools`, `xorriso`).

```bash
cmake --build build --target image   # -> build/ISO/os.iso (tars the initrd, runs grub-mkrescue)
./scripts/run-qemu.sh                 # headless QEMU, kernel COM1 debug on stdio
```

`run-qemu.sh` documents its env knobs (`MACHINE`, default `q35` — needed for
the ACPI MCFG table the PCI code requires; `GPU`, `MEM`, `ACCEL`, `SCREENSHOT`,
`TIMEOUT`, …). The CMake `run` target (`qemu-virgil` + GTK/virgl) is for an
interactive 3D desktop and is not used for smoke tests.

### CI

`.github/workflows/build.yml` reuses `scripts/devenv/environment.yml` as the
single source of truth for the toolchain, builds host tools + target, and
asserts `kernel.bin` exists and ≥24 `.celf` modules were produced. Keep that
module count in mind when adding/removing modules (the tree currently builds
~28 `.celf`s — down from ~40 after the USB stack moved to Lisp and the orphaned
C drivers were removed).

## Repository layout

| Path | Role |
|------|------|
| `kernel/` | The tiny core: ELF/relocatable loader, initrd (tar) parsing, boot-script interpreter, bootstrap allocator, symbol DB, DWARF. Linked at a fixed high virtual address. |
| `modules/` | `Sys*` kernel-privileged modules: memory (`SysPhysicalMemory`, `SysVirtualMemory`, `SysMemory`), `SysInterrupts`, `SysMP`, `SysTimer`, `SysFP`, `SysObj` (object model), `SysReg` (registry), `SysUser` (syscalls), `SysTaskMgr` (scheduler), `SysDebug` (serial I/O, the per-source log store read by the REPL's `log-*` prims, and the panic path). |
| `lisp/` | The OS above the `Sys*` core is **Lisp**, run by the kernel-resident bytecode VM (`libs/lisp`, compiled into `modules/SysLisp`). `lisp/servers/*.clp` are the `Core*` services (`coreinput`, `coreaudio`, `corepower`, `corestorage` + `cardfs`, `coredisplay`, `corenetwork` — ARP/ICMP/IPv4 + UDP + the reliable `RDT` transport — `corenetdebug`, `coreusb`); `lisp/drivers/*.clp` are the drivers (`ps2`, `rtl8139`, `virtio-net`, `virtio-gpu`, `lfb`, `ahci`, `hdaudio`, `rtl8169`, the USB stack `uhci`/`xhci` + `usb-{hid,hub,storage}`); `lisp/lib/` is the prelude/substrate; **`lisp/init.clp`** is the boot policy and sole device binder (it `pci-find`s hardware and calls each driver's init). The old C `servers/` tree and the C `CoreDriver`/`devices.txt` binder were deleted. |
| `drivers/` | Only `tarfs` remains in C (a stub). Every actively-bound driver is Lisp under `lisp/drivers/` (see the `lisp/` row); the orphaned C drivers (`rtl8169`/`intel_wifi`/`intel_gfx`/`hdaudio`/`ehci`/`usb_*`/`uhci`/`xhci`, plus the now-Lisp `virtio`/`rtl8139`/`lfb`/`ahci`/`ps2`/`cardfs`) were removed. |
| `libs/` | Static libs linked into modules: `crypto` (sha256/hmac), `miniz`, `module_lib` (CELF header build/verify), `kvs`, `ubsan_handlers`, plus header-only `pci/` and `syscalls/`. `pci/` holds `pci.h` (config space, BAR scan), `pci_irq.h` (MSI/MSI-X setup), `pci_alloc.h` (BAR + bridge-window self-assignment for firmware-unconfigured devices), `pci_debug.h` (`pci_msix_debug_dump`). |
| `common/` | Freestanding mini-libc (`string`, `stdlib`, `stdio`, lists/queues, `time`) + platform type headers. Included as a SYSTEM include everywhere. |
| `platform/<isa>/<plat>/` | Per-target CMake fragments (`flags.cmake`), `linker.ld`, GRUB configs, and the `image`/`run` custom targets. |
| `utils/sign_exec/` | **Host** tool that wraps an ELF in a signed `ModuleHeader` → `.celf`. |
| `mana/` | `mana.celf`, the desktop environment / first userspace task. |
| `notes/` | Design docs (`core/`, `servers/`, `drivers/`) and **`notes/AUDIT.md`** — a tracked list of known bugs/stubs. Read it before touching the display stack, memory, or USB. |
| `scripts/` | `build.sh`, `run-qemu.sh`, `style_com.sh`, and `devenv/`. |

## How modules work (the core concept)

- A module is a **relocatable ELF** (linked with `-r`). The kernel finds its
  own symbol table and resolves each module's imports against the symbols
  already loaded, so **load order matters** (see boot scripts below).
- Every module exposes an `int module_init(...)` entry point. System
  modules/servers take no args; PCI **drivers** take `void *ecam` (the device's
  ECAM config space). Return `0` on success.
- `sign_exec` wraps the ELF in a `ModuleHeader` (`libs/module_lib/module_def.h`)
  containing name, NID, version, an HMAC-SHA256 over header+payload, and a
  truncated key hash, producing a `.celf`. The kernel runs `VerifyModule`
  before loading — this is the security boundary.
- **Two signing keys** (plain-text hex in repo root, read by the root
  CMakeLists):
  - `KMOD_HMAC_Key.txt` — used by **`modules/` (`Sys*`)**.
  - `SERV_HMAC_Key.txt` — used by **`drivers/`** (and formerly `servers/`, now Lisp).
  Regenerate with `printf <source> | xxd -pu > KMOD_HMAC_Key.txt`.

### Boot-time scripts (in the repo root, copied into the initrd)

The kernel reads these text scripts; `LOAD:` loads a `.celf`, `CALL:` invokes an
already-resolved exported function by name.

- `loadscript.txt` — bring-up order for the `Sys*` modules (memory → interrupts
  → MP → timer → object/user/taskmgr), ending with `LOAD:./SysLisp.celf` +
  `CALL:lisp_scheduler_enter` (which never returns — the boot thread becomes the
  per-core Lisp scheduler loop).
- `apscript.txt` — per-AP (application processor) init sequence for SMP.

The old C service/driver binding (`servicescript.txt` + `devices.txt` read by
`CoreDriver`) is **gone**. Servers and drivers are now Lisp modules under `lisp/`,
and **`lisp/init.clp`** is the single place boot policy lives: it brings up the
`Core*` services and binds drivers to hardware (each gated on `pci-find`). The
kernel only `(import init)`s it and calls `(system-init)`.

When you add a *Sys\** module you touch its `CMakeLists.txt`, the parent
`CMakeLists.txt` (`ADD_SUBDIRECTORY`), and `loadscript.txt`. When you add a
Lisp server/driver you add its `.clp` under `lisp/` (auto-packaged into the
initrd) and wire its bring-up into `lisp/init.clp` — no CMake/boot-script change.

## Conventions

### CMake (per-module pattern)

Each module/server/driver `CMakeLists.txt` follows the same shape (see
`modules/SysDebug/CMakeLists.txt` or `drivers/virtio/gpu/CMakeLists.txt`):

```cmake
SET(CELF_NAME <Name>)
FILE(GLOB SRCS          ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c)
FILE(GLOB ISA_SRCS      ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/${CUR_ISA}/*.c)
FILE(GLOB PLATFORM_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/${CUR_ISA}/${CUR_PLATFORM}/*.c)
ADD_EXECUTABLE(${CELF_NAME}.elf ...)
ADD_CUSTOM_TARGET(${CELF_NAME}.celf ALL ...  # invokes ${CELF_GEN} with the right HMAC key, copies to ${PLATFORM_CELF_DIR}
SET_TARGET_PROPERTIES(... COMPILE_OPTIONS "-fno-pic")
SET_TARGET_PROPERTIES(... LINK_FLAGS "-r ${ISA_LINKER_FLAGS} ${PLATFORM_LINKER_FLAGS}")
```

Sources are **globbed**, so a new `.c` in `src/` (or the matching
`src/platform/<isa>[/<plat>]/`) is picked up after re-configure — but globs
don't auto-detect new files on an incremental Ninja build; re-run `cmake -S . -B build`.

Platform-specific code lives under `src/platform/<isa>/` and
`src/platform/<isa>/<plat>/`; the linker order is common → ISA → platform so
platform code can override weak symbols. Use `CUR_ISA` / `CUR_PLATFORM`
(set by `SET_PLATFORM` in the root CMakeLists, currently `x86_64`/`pc`).

### Compiler flags (don't fight these)

Defined in `platform/x86_64/flags.cmake`. Notably: `-ffreestanding -nostdinc`,
`-mno-red-zone`, `-mcmodel=large`, **all SSE/MMX disabled** (`-mno-sse …` —
no floating point in kernel modules), `-Werror` with several `-Wno-unused-*`.
The kernel links `-static` (clang would otherwise emit a PIE and reject the
absolute relocations). To change the target, edit `SET_PLATFORM(ARCH, PLATFORM)`
in the root `CMakeLists.txt`.

### Code style

Google style via astyle: `astyle -r -n --style=google "*.c" "*.h"`
(`scripts/style_com.sh`). Match the surrounding file. New source files carry the
MIT copyright header used across the tree:

```c
// Copyright (c) <year> Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT
```

### Inter-module APIs

Public headers live in `modules/inc/Sys*/` and driver `inc/` dirs, included via
the `TARGET_INCLUDE_DIRECTORIES` lists (the C `servers/inc/Core*/` headers are
gone — the `Core*` services are Lisp now and talk over message protocols, not C
ABIs). Key
facilities: the **registry** (`SysReg` — a hierarchical key/value store,
`registry_addkey_*`/`registry_readkey_*`), the **object model** (`SysObj`),
**syscalls** (`libs/syscalls/cs_syscall.h`, register-based `syscallq`), and
driver→server registration (e.g. a display driver calls `display_register()`
to attach to `CoreDisplay`, a NIC calls `network_register()` to attach to
`CoreNetwork`). Debug output is `DEBUG_PRINT(...)` over COM1.

**PCI drivers** receive their device's ECAM (`module_init(void *ecam)`) and map
it as a `pci_config_t`. The usual path is `pci_first_mmio_bar()` (firmware
assigned the BARs); a device firmware never used at boot (e.g. an onboard NIC
behind a PCIe root port) may have *unassigned* BARs and a *closed* bridge window
— call `pci_assign_bars()` (`libs/pci/pci_alloc.h`) to place its BARs and open
every bridge window up to the root bus. Set up MSI(-X) with
`pci_setup_msi_handler()` (`pci_irq.h`), which registers the handler *before*
enabling the capability; enable the device's own interrupt mask **last**, after
the handler/poll task exist (otherwise an edge-triggered MSI can fire and be lost
before anything is listening, wedging interrupts).

**Driver gotcha — RX-handler locking:** `network_rx_packet()` runs the network
stack synchronously and, for a request that needs a reply (ARP/ICMP, or a UDP
service that answers via `udp_send_to`), re-enters the *same* driver's TX path.
Never hold a driver lock across `network_rx_packet` or you self-deadlock the
moment a reply-triggering frame arrives. The same rule applies inside CoreNetwork:
the UDP/RDT layers copy a handler out from under their table lock and release it
before invoking it, since the handler may reply.

## Git workflow for this environment

- The default branch is `master`. Do all work on a short-lived feature branch
  (e.g. `claude/<topic>`) cut from `master`; never commit or push to `master`
  without explicit permission.
- **Push each feature branch as you commit it** (`git push -u origin <branch>`)
  so the work is reviewable, and open a **draft** PR if one doesn't already
  exist (set the PR base to the parent branch for stacked work, so each PR shows
  only its own diff). Once a PR is merged, delete its branch.

## Gotchas

- **Two builds, two compilers**: `utils_build/` is host, `build/` is the
  `x86_64-elf` target. Don't cross them.
- **`-fno-pic` / `-static`** are load-bearing for the relocatable model; don't
  "fix" them away.
- **No floating point / no libc** in kernel-space code — use `common/`.
- **Load order** is explicit in the boot scripts; an unresolved import means a
  module is loaded before the one that exports the symbol.
- `notes/AUDIT.md` enumerates known stubs and which are now addressed
  (`CoreAudio`/`tarfs` `module_init` still empty; `CoreStorage` now has a
  block-device registry + the `cardfs` object-store exploration; `CoreNetwork`
  ARP/ICMP/IPv4 + UDP (port bind/send) + reliable transport (`RDT`) work, TCP and
  the *userspace* socket API still TODO; the USB stack (now Lisp) enumerates over
  UHCI/xHCI with HID/hub/mass-storage class drivers). Check it before assuming
  something is broken vs. intentionally unfinished.
- **Boot timing knobs that bit recent work** (detailed in `notes/AUDIT.md`):
  `task_sleep` itself is now fixed and reliable for
  normal delays (it actually deschedules — see AUDIT), but it can't be used by
  code already holding `cli()` (e.g. AHCI init), which still busy-spins via
  the TSC-calibrated `SysTimer` waits; the boot-script files
  (`loadscript.txt`/`apscript.txt`) use **CRLF** line endings — keep them
  CRLF or the parser panics with "Unknown Command".

## Debugging

The interactive debug path is the **in-OS serial Lisp REPL** over COM1 (the old
`SysGdb` GDB-over-serial stub was removed in favour of it). Boot with the
`cardinal.repl` cmdline flag (the `repl-image` CMake target → `build/ISO/os-repl.iso`)
and drive it with **`scripts/serial-repl.py`** (interactive, or `--exec "<lisp>"`
to script it). Without `cardinal.repl` the boot log streams raw to COM1 as usual
(what CI and the boot smoke-tests read); with it, once `start-repl`
(`lisp/init.clp`) runs, the REPL **takes** COM1 and component logs move to the
in-memory **log store** — read them back with `(log-sources)` / `(log-dump
"<source>")` / `(log-tail "<source>" n)`. Each module logs under its own source
via `(log "src" …)` or a `(make-logger 'src)` closure; the store is C
(`SysDebug/logstore.c`) for synchronous, allocation-free capture. The REPL also
imports the reflective debugger (`sys-debug`: `ctx-list`/`ctx-pause`/`ctx-step`/…)
to inspect and single-step live contexts. The full boot works under `-accel kvm`
(fast) thanks to the APIC-timer ordering fix; before that it booted only under TCG.
