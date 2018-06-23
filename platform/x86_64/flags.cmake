
FILE(GLOB ISA_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/kernel/src/platform/x86_64/*.c")

SET(ISA_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/kernel/inc/platform/x86_64/")

SET(ISA_LINKER_EXEC "clang")
SET(ISA_ASM_COMPILER "clang")

SET(ISA_C_FLAGS "-fsanitize=undefined,null -target x86_64-none-elf -nostdinc -std=c11 -ffreestanding -Wall -Wextra -Wno-unused-variable -Wno-trigraphs -Werror -mno-red-zone -mcmodel=kernel -mno-aes -mno-mmx -mno-pclmul -mno-sse -mno-sse2 -mno-sse3 -mno-sse4 -mno-sse4a -mno-fma4 -mno-ssse3")
SET(ISA_ASM_FLAGS "")
SET(ISA_DEFINITIONS "")
SET(ISA_LINKER_FLAGS "-ffreestanding -target x86_64-none-elf -O2 -mno-red-zone -nostdlib -z max-page-size=0x1000 -mcmodel=kernel")