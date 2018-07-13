# Cardinal;
An extremely modular, security oriented microkernel operating system based on a cleanup and partial rewrite of the Cardinal operating system.

## Building
CMake configuration command, for out-of-source builds:
```bash
CC=clang cmake -DCMAKE_BUILD_TYPE=Debug -DCMAKE_SYSTEM_NAME="Generic" -D_CMAKE_TOOLCHAIN_PREFIX=llvm- ..
```

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

##Device Support Status

###AHCI
Port from Cardinal, on-hold until object model is fully fleshed out.

###Intel HD Graphics
Studying PRMs and testing display initialization and mode set for Haswell.

###Intel HD Audio
Node enumeration working, path-finding and CoreAudio development to go.

###Intel WiFi
No driver code yet, studying FreeBSD iwm driver and 802.11 specification. Expecting to start work after Network stack is minimally functional.

###Linear Framebuffer
Driver implemented, acts as fallback display driver.

###PS/2
Keyboard support working, Mouse support bugged. Does not register to CoreInput yet.

###RTL8139
Development dropped due to lack of MSI support.

###VirtioGpu
Works if not used in conjunction with VirtioNet, registers properly with CoreDisplay. 3d Acceleration not available yet.

###VirtioNet
Works if not used in conjunction with VirtioGpu, registers with CoreNetwork.
