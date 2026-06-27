# Cardinal;
An extremely modular, security oriented microkernel operating system based on a cleanup and partial rewrite of the Cardinal operating system.

## Building

The toolchain is **Clang/LLVM** targeting freestanding `x86_64-elf` (clang is a
native cross compiler, so no separate GCC/binutils cross build is needed). A
repo-local conda environment under `scripts/devenv/` provides clang, lld,
llvm-tools, cmake and ninja.

```bash
# One-time: build the local toolchain environment (./.devenv)
./scripts/devenv/setup.sh

# Each build session: put the toolchain on PATH
source ./scripts/devenv/activate.sh

# Build everything (host sign_exec tool + kernel + modules)
./scripts/build.sh
```

`scripts/build.sh` is a thin wrapper around the two CMake builds it runs:

```bash
# 1. host-side signing tool
cmake -S utils -B utils_build -G Ninja && cmake --build utils_build
# 2. the OS itself
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_SYSTEM_NAME=Generic -DCMAKE_C_COMPILER=clang -DCMAKE_ASM_COMPILER=clang
cmake --build build
```

Artifacts land in `build/`: `kernel/kernel.bin` and the signed `*.celf` modules
(also staged under `build/ISO/isodir/boot/`).

See `scripts/devenv/README.md` for details and the optional system packages
(qemu/grub/mtools/xorriso) needed to build a bootable ISO.

To generate a custom kmod signing key, use:
```bash
printf kerneltest0 | xxd -pu > KMOD_HMAC_Key.txt
```
replacing kerneltest0 with your desired source string

## Changing the target
To set the target, in the root CMakeLists.txt:
SET_PLATFORM( ARCH, PLATFORM )

Possible values for ARCH:
- "x86_64"

Possible values for PLATFORM:
- "pc"

## Device Support Status

Every actively-bound driver is Lisp under `lisp/drivers/`, bound to hardware by
`lisp/init.clp` (gated on `pci-find`). Status legend: **✅ working**,
**🟡 partial / real-hardware-only**, **⬜ not implemented**.

### Storage (register block devices with CoreStorage)

| Device | Status | Notes |
|--------|--------|-------|
| AHCI / SATA | ✅ | Reset, IDENTIFY, read/write; registers disks with CoreStorage. |
| NVMe | ✅ | Admin + I/O queue pair, IDENTIFY, read/write (phase-bit polling); binds by PCI class 01h/08h. |
| virtio-blk | ✅ | Single request queue; registers with CoreStorage. |

### Networking (register NICs with CoreNetwork — DHCP/ARP/IPv4/UDP/TCP)

| Device | Status | Notes |
|--------|--------|-------|
| virtio-net | ✅ | Full stack; gets a DHCP lease. Coexists with virtio-gpu (the old "one at a time" bug is fixed). |
| Intel e1000 / e1000e | ✅ | 82540EM + 82574L; full DHCP (RX+TX) on both via a poll-based RX pump (82540 has no MSI; 82574 needs MSI-X IVAR). igb deferred. |
| RTL8168/8111 (`rtl8169`) | 🟡 | Real hardware only — QEMU emulates no 8168. TX works; RX physically flaky. |
| RTL8139 | 🟡 | Brings up but has no working RX path — prefer virtio-net. |
| Intel WiFi | ⬜ | No driver yet; studying the FreeBSD `iwm` driver + 802.11. |

### Display / GPU (register with CoreDisplay)

| Device | Status | Notes |
|--------|--------|-------|
| virtio-gpu | ✅ | Registers a scanout and backs the compositor; coexists with virtio-net. No 3D acceleration yet. |
| Linear framebuffer (`lfb`) | ✅ | Fallback display over the firmware framebuffer; WC-mapped, double-buffered compose. |
| Intel HD Graphics | ⬜ | Studied (Haswell/Cherrytrail mode-set) but no driver is bound; no bare-metal mode-set. |

### Input (feed CoreInput)

| Device | Status | Notes |
|--------|--------|-------|
| PS/2 | ✅ kbd / 🟡 mouse | Keyboard works and registers with CoreInput. Mouse: i8042 aux bring-up + packet decode done and host-unit-tested, but QEMU does not inject PS/2 mouse input, so the live pointer path is real-hardware-only. |
| USB HID | ✅ | Keyboard/mouse over the USB stack. |
| virtio-input | ✅ | Tablet (absolute pointer) + keyboard register with CoreInput. (Keyboard emits evdev keycodes — an evdev→set-1 mapping is a follow-up.) |

### Audio (register with CoreAudio)

| Device | Status | Notes |
|--------|--------|-------|
| Intel HD Audio | ✅ | Full stack: endpoint model, multi-controller, per-endpoint volume/mute, capture, codec hotplug, jack-presence sense. |
| USB Audio (UAC1) | ✅ | Streams tones over the iso OUT endpoint. |

### USB

| Component | Status | Notes |
|-----------|--------|-------|
| Controllers: UHCI / xHCI / EHCI | ✅ | Enumerate over the Lisp USB stack (CoreUsb). |
| Class drivers: HID / hub / mass-storage / audio | ✅ | Mass-storage registers disks with CoreStorage; HID feeds CoreInput. |

### Misc

| Device | Status | Notes |
|--------|--------|-------|
| virtio-rng | ✅ | Entropy service answering `(get-random n)`. |
| virtio-console | ✅ | Single-port serial console (transmit; banner on bring-up). |
