cmake_minimum_required(VERSION 3.20)

SET(CELF_NAME SysLisp)

FILE(GLOB SRCS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c)
FILE(GLOB ISA_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/${CUR_ISA}/*.c)
FILE(GLOB PLATFORM_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/${CUR_ISA}/${CUR_PLATFORM}/*.c)

ADD_EXECUTABLE(${CELF_NAME}.elf "${SRCS}" "${ISA_SRCS}" "${PLATFORM_SRCS}")

ADD_CUSTOM_TARGET(${CELF_NAME}.celf ALL
    DEPENDS ${CELF_NAME}.elf
    COMMAND ${CELF_GEN} Cardinal_${CELF_NAME} Himanshu Goel 0000 0000 ${KMOD_HMAC_Key} ${CELF_NAME}.elf -o ${CELF_NAME}.celf
    COMMAND mkdir -p ${PLATFORM_CELF_DIR}
    COMMAND cp -t ${PLATFORM_CELF_DIR} ${CELF_NAME}.celf
)

# SSE is re-enabled here (overriding the kernel's blanket -mno-sse) to match the
# `lisp`/`ttf` libraries this module links: the Scheme runtime uses native
# hardware doubles for flonums, and the sys-wasm glue must coerce f32/f64 guest
# values to/from those flonums. Safe because SysLisp runs in task context with FP
# state saved per switch, and module_init runs after SysFP (the loadscript loads
# SysFP before SysLisp). -mstackrealign: the kernel/module call path is -mno-sse
# and does not guarantee the 16-byte stack alignment SSE's aligned spills assume,
# so each SSE function realigns its own stack (matches libs/lisp/CMakeLists.txt).
SET_TARGET_PROPERTIES(${CELF_NAME}.elf PROPERTIES COMPILE_OPTIONS "-fno-pic;-msse;-msse2;-mstackrealign")
TARGET_INCLUDE_DIRECTORIES(${CELF_NAME}.elf PRIVATE "inc" "../inc" "../../kernel/inc" "${LIBS_DIR}/lisp/inc" "${LIBS_DIR}/syscalls" "${LIBS_DIR}/ttf" "${LIBS_DIR}/wasm/inc" "${LIBS_DIR}")
TARGET_INCLUDE_DIRECTORIES(${CELF_NAME}.elf SYSTEM PUBLIC "${KERN_STDLIB_INCLUDE_DIR}")
# Links the kernel-resident Scheme runtime (built freestanding with SSE re-enabled
# for its hardware-double flonums) plus the UBSan handlers used across modules and
# `ttf` (stb_truetype, likewise SSE-enabled) for runtime TrueType glyph rasterization.
TARGET_LINK_LIBRARIES(${CELF_NAME}.elf PRIVATE lisp ttf wasm ubsan_handlers)
SET_TARGET_PROPERTIES(${CELF_NAME}.elf PROPERTIES LINK_FLAGS "-r ${ISA_LINKER_FLAGS} ${PLATFORM_LINKER_FLAGS}")
