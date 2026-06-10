cmake_minimum_required(VERSION 3.20)

FILE(GLOB SIGNEXEC_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c)

ADD_EXECUTABLE(sign_exec "${SIGNEXEC_SRCS}")
TARGET_INCLUDE_DIRECTORIES(sign_exec PUBLIC ${LIBS_DIR}/miniz  ${LIBS_DIR}/module_lib ${LIBS_DIR}/crypto)

TARGET_LINK_LIBRARIES(sign_exec PUBLIC cardinal_module crypto miniz)