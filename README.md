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

### AHCI
Port from Cardinal, on-hold until object model is fully fleshed out.

### Intel HD Graphics
Studying PRMs and testing display initialization and mode set for Haswell.

### Intel HD Audio
Node enumeration working, path-finding and CoreAudio development to go.

### Intel WiFi
No driver code yet, studying FreeBSD iwm driver and 802.11 specification. Expecting to start work after Network stack is minimally functional.

### Linear Framebuffer
Driver implemented, acts as fallback display driver.

### PS/2
Keyboard support working, Mouse support bugged. Does not register to CoreInput yet.

### RTL8169
WIP, Minimum required functionality for all RTL8169 based NICs.

### RTL8139
Development dropped due to lack of MSI support.

### VirtioGpu
Works; registers a scanout with CoreDisplay and backs the compositor. Coexists
with VirtioNet — both bring up and run concurrently (verified under QEMU q35: a
virtio-net DHCP lease and a virtio-gpu scanout come up together). The earlier
"only one at a time" limitation is gone (it was an interrupt-delivery bug, since
fixed); virtio-gpu polls its completion ring and uses no MSI, while each NIC
takes its own exclusive MSI vector, so they cannot contend. 3D acceleration not
available yet.

### VirtioNet
Works; registers with CoreNetwork (DHCP/ARP/IPv4/UDP/TCP functional). Coexists
with VirtioGpu (see above).
