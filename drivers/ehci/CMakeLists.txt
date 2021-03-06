CMAKE_MINIMUM_REQUIRED(VERSION 2.8)

SET(CELF_NAME ehci)

FILE(GLOB SRCS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c)
FILE(GLOB ISA_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/${CUR_ISA}/*.c)
FILE(GLOB PLATFORM_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/${CUR_ISA}/${CUR_PLATFORM}/*.c)

ADD_EXECUTABLE(${CELF_NAME}.elf "${SRCS}" "${ISA_SRCS}" "${PLATFORM_SRCS}")

ADD_CUSTOM_TARGET(${CELF_NAME}.celf ALL
    DEPENDS ${CELF_NAME}.elf
    COMMAND ${CELF_GEN} Cardinal_${CELF_NAME} Himanshu Goel 0000 0000 ${SERV_HMAC_Key} ${CELF_NAME}.elf -o ${CELF_NAME}.celf
    COMMAND mkdir -p ${PLATFORM_CELF_DIR}
    COMMAND cp -t ${PLATFORM_CELF_DIR} ${CELF_NAME}.celf
)

SET_TARGET_PROPERTIES(${CELF_NAME}.elf PROPERTIES COMPILE_OPTIONS "-fno-pic")
TARGET_INCLUDE_DIRECTORIES(${CELF_NAME}.elf PRIVATE "inc" "../../modules/inc" "../../servers/inc" "../../libs" "${LIBS_DIR}/syscalls")
TARGET_INCLUDE_DIRECTORIES(${CELF_NAME}.elf SYSTEM PUBLIC "${KERN_STDLIB_INCLUDE_DIR}")

SET_TARGET_PROPERTIES(${CELF_NAME}.elf PROPERTIES LINK_FLAGS "-r ${ISA_LINKER_FLAGS} ${PLATFORM_LINKER_FLAGS}")