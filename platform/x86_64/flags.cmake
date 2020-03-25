
FILE(GLOB ISA_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/kernel/src/platform/x86_64/*.c")

SET(ISA_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/kernel/inc/platform/x86_64/")

SET(ISA_LINKER_EXEC "x86_64-elf-cardinalsemi-gcc")
SET(ISA_ASM_COMPILER "x86_64-elf-cardinalsemi-gcc")

SET(ISA_C_FLAGS "-fsanitize=undefined,null -fPIC -nostdinc -std=gnu11 -ffreestanding -Wall -Wextra -Wno-unused-but-set-variable -Wno-unused-but-set-parameter -Wno-unused-variable -Wno-trigraphs -Werror -mno-red-zone -mcmodel=large -mno-aes -mno-mmx -mno-pclmul -mno-sse -mno-sse2 -mno-sse3 -mno-sse4 -mno-sse4a -mno-fma4 -mno-ssse3")
SET(ISA_ASM_FLAGS "-fPIC")
SET(ISA_DEFINITIONS "")
SET(ISA_LINKER_FLAGS "-ffreestanding -O2 -mno-red-zone -nostdlib -z max-page-size=0x1000 -mcmodel=large")