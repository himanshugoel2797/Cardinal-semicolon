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
asserts `kernel.bin` exists and ≥30 `.celf` modules were produced. Keep that
module count in mind when adding/removing modules.

## Repository layout

| Path | Role |
|------|------|
| `kernel/` | The tiny core: ELF/relocatable loader, initrd (tar) parsing, boot-script interpreter, bootstrap allocator, symbol DB, DWARF. Linked at a fixed high virtual address. |
| `modules/` | `Sys*` kernel-privileged modules: memory (`SysPhysicalMemory`, `SysVirtualMemory`, `SysMemory`), `SysInterrupts`, `SysMP`, `SysTimer`, `SysFP`, `SysObj` (object model), `SysReg` (registry), `SysUser` (syscalls), `SysTaskMgr` (scheduler), `SysDebug`. |
| `servers/` | `Core*` OS services: `CoreDisplay`, `CoreAudio`, `CoreInput`, `CoreNetwork`, `CoreStorage`, `CoreUsb`, `CorePower`, `CoreDriver`. |
| `drivers/` | Device drivers: `virtio` (gpu/net/common), `intel_gfx`, `intel_wifi`, `hdaudio`, `rtl8139`, `rtl8169`, `ahci`, `uhci`, `ehci`, `ps2`, `lfb`, `tarfs`. |
| `libs/` | Static libs linked into modules: `crypto` (sha256/hmac), `miniz`, `module_lib` (CELF header build/verify), `kvs`, `ubsan_handlers`, plus header-only `pci/` and `syscalls/`. |
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
  - `SERV_HMAC_Key.txt` — used by **`servers/` and `drivers/`**.
  Regenerate with `printf <source> | xxd -pu > KMOD_HMAC_Key.txt`.

### Boot-time scripts (in the repo root, copied into the initrd)

The kernel reads these text scripts; `LOAD:` loads a `.celf`, `CALL:` invokes an
already-resolved exported function by name.

- `loadscript.txt` — bring-up order for the `Sys*` modules (memory → interrupts
  → MP → timer → object/user/taskmgr), interleaved with `CALL:` init steps.
- `apscript.txt` — per-AP (application processor) init sequence for SMP.
- `servicescript.txt` — loads the `Core*` servers and device drivers.
- `devices.txt` — PCI match table: `./<driver>.celf|VID|DID|class|subclass|progif`
  (`FFFF`/`00` = wildcard). This is how drivers get bound to hardware.

When you add a module/server/driver, you almost always also touch its
`CMakeLists.txt`, the parent `CMakeLists.txt` (`ADD_SUBDIRECTORY`), and the
relevant boot script (and `devices.txt` for a PCI driver).

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

Public headers live in `modules/inc/Sys*/`, `servers/inc/Core*/`, and driver
`inc/` dirs, included via the `TARGET_INCLUDE_DIRECTORIES` lists. Key
facilities: the **registry** (`SysReg` — a hierarchical key/value store,
`registry_addkey_*`/`registry_readkey_*`), the **object model** (`SysObj`),
**syscalls** (`libs/syscalls/cs_syscall.h`, register-based `syscallq`), and
driver→server registration (e.g. a display driver calls `display_register()`
to attach to `CoreDisplay`). Debug output is `DEBUG_PRINT(...)` over COM1.

## Git workflow for this environment

- The default branch is `master`. Do all work on a short-lived feature branch
  (e.g. `claude/<topic>`) cut from `master`; never commit or push to `master`
  without explicit permission.
- Push with `git push -u origin <branch>`; after pushing, open a **draft** PR if
  one doesn't already exist. Once a PR is merged, delete its branch.
- `git commit`/`push` only when asked.

## Gotchas

- **Two builds, two compilers**: `utils_build/` is host, `build/` is the
  `x86_64-elf` target. Don't cross them.
- **`-fno-pic` / `-static`** are load-bearing for the relocatable model; don't
  "fix" them away.
- **No floating point / no libc** in kernel-space code — use `common/`.
- **Load order** is explicit in the boot scripts; an unresolved import means a
  module is loaded before the one that exports the symbol.
- `notes/AUDIT.md` enumerates known stubs (`CoreAudio`/`CoreStorage`/`tarfs`
  `module_init` are empty; `CoreNetwork` ARP/IP/TCP TODO; Haswell `intel_gfx`
  largely absent; several unbounded hardware busy-waits). Check it before
  assuming something is broken vs. intentionally unfinished.
