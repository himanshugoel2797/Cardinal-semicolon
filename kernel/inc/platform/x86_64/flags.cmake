
FILE(GLOB ISA_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/x86_64/*.c")

SET(ISA_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/inc/platform/x86_64/")
SET(ISA_ASM_COMPILER "x86_64-elf-as")
SET(ISA_C_FLAGS "-mcmodel=kernel -target x86_64-none-elf -ffunction-sections -nostdlib --target=x86_64-none-elf -nostdinc -std=c11 -ffreestanding -Wall -Werror")