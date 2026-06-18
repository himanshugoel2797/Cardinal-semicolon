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

SET_TARGET_PROPERTIES(${CELF_NAME}.elf PROPERTIES COMPILE_OPTIONS "-fno-pic")
TARGET_INCLUDE_DIRECTORIES(${CELF_NAME}.elf PRIVATE "inc" "../inc" "../../kernel/inc" "${LIBS_DIR}/lisp/inc" "${LIBS_DIR}/syscalls")
TARGET_INCLUDE_DIRECTORIES(${CELF_NAME}.elf SYSTEM PUBLIC "${KERN_STDLIB_INCLUDE_DIR}")
# Links the kernel-resident Scheme runtime (built freestanding with SSE re-enabled
# for its hardware-double flonums) plus the UBSan handlers used across modules.
TARGET_LINK_LIBRARIES(${CELF_NAME}.elf PRIVATE lisp ubsan_handlers)
SET_TARGET_PROPERTIES(${CELF_NAME}.elf PROPERTIES LINK_FLAGS "-r ${ISA_LINKER_FLAGS} ${PLATFORM_LINKER_FLAGS}")
