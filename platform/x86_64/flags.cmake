
FILE(GLOB ISA_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/kernel/src/platform/x86_64/*.c")

SET(ISA_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/kernel/inc/platform/x86_64/")

# Clang is the cross compiler: it is natively multi-target, so we point it at a
# bare freestanding x86_64 ELF target (no OS, no libc). lld is the linker. The
# linker is driven through clang (not invoked directly) so it honours --target,
# -nostdlib and friends.
SET(ISA_TARGET "x86_64-elf")
SET(ISA_LINKER_EXEC "clang --target=${ISA_TARGET} -fuse-ld=lld")
SET(ISA_ASM_COMPILER "clang")

SET(ISA_C_FLAGS "--target=${ISA_TARGET} -g -fno-plt -fno-pic -fno-stack-protector -nostdinc -std=gnu11 -ffreestanding -Wall -Wextra -Wno-unused-label -Wno-unused-function -Wno-unused-but-set-variable -Wno-unused-but-set-parameter -Wno-unused-variable -Wno-trigraphs -Werror -mno-red-zone -mcmodel=large -mno-aes -mno-mmx -mno-pclmul -mno-sse -mno-sse2 -mno-sse3 -mno-sse4 -mno-sse4a -mno-fma4 -mno-ssse3")
SET(ISA_ASM_FLAGS "--target=${ISA_TARGET} -fno-plt")
SET(ISA_DEFINITIONS "")
# -static: the kernel is linked at a fixed high virtual address with absolute
# (non-PIC) relocations. clang would otherwise drive lld to produce a PIE and
# reject the R_X86_64_32/64 relocations ("recompile with -fPIC"); clang ignores
# -no-pie for the bare x86_64-elf target, so -static is what forces a plain
# statically-linked, non-relocatable executable.
SET(ISA_LINKER_FLAGS "-ffreestanding -O2 -mno-red-zone -static -nostdlib -Wl,-z,max-page-size=0x1000 -mcmodel=large")

# Use the LLVM binutils replacements for archiving (clang toolchain).
FIND_PROGRAM(LLVM_AR NAMES llvm-ar)
FIND_PROGRAM(LLVM_RANLIB NAMES llvm-ranlib)
IF(LLVM_AR)
    SET(CMAKE_AR "${LLVM_AR}" CACHE FILEPATH "LLVM archiver" FORCE)
ENDIF()
IF(LLVM_RANLIB)
    SET(CMAKE_RANLIB "${LLVM_RANLIB}" CACHE FILEPATH "LLVM ranlib" FORCE)
ENDIF()