# Cardinal;
An extremely modular, security oriented microkernel operating system based on a cleanup and partial rewrite of the Cardinal operating system.

## Building
CMake configuration command, for out-of-source builds:
```bash
CC=clang cmake -DCMAKE_SYSTEM_NAME="Generic" ..
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