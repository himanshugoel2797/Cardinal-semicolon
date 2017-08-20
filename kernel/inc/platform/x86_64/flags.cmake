
FILE(GLOB ISA_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/x86_64/*.c")

SET(ISA_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/inc/platform/x86_64/")

SET(ISA_LINKER_EXEC "x86_64-elf-gcc")
SET(ISA_ASM_COMPILER "x86_64-elf-gcc")

SET(ISA_C_FLAGS "-g -fsanitize=undefined -fsanitize-trap=undefined -ftrap-function=debug_handle_trap -target x86_64-none-elf -nostdinc -std=c11 -ffreestanding -Wall -Wextra -Wno-unused-variable -Wno-trigraphs -Werror -D_KERNEL_ -mno-red-zone -mcmodel=kernel -O0 -mno-aes -mno-mmx -mno-pclmul -mno-sse -mno-sse2 -mno-sse3 -mno-sse4 -mno-sse4a -mno-fma4 -mno-ssse3")
SET(ISA_ASM_FLAGS "")
SET(ISA_LINKER_FLAGS "-ffreestanding -O2 -mno-red-zone -nostdlib -z max-page-size=0x1000 -mcmodel=kernel")