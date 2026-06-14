cmake_minimum_required(VERSION 3.20)
project(cardinal_tests C)

# Host-side unit tests for the freestanding-but-host-compilable leaf code.
# Built with the HOST compiler (no cross flags) -- see scripts/run-tests.sh.

enable_testing()

SET(LIBS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../libs")

# Reuse the real crypto sources (the same ones sign_exec links).
ADD_SUBDIRECTORY("${LIBS_DIR}/crypto" ${CMAKE_CURRENT_BINARY_DIR}/crypto)

FILE(GLOB TEST_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/*.c)

ADD_EXECUTABLE(cardinal_tests "${TEST_SRCS}")
TARGET_INCLUDE_DIRECTORIES(cardinal_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${LIBS_DIR}/crypto"
    "${CMAKE_CURRENT_SOURCE_DIR}/../servers/CoreNetwork/inc")
TARGET_LINK_LIBRARIES(cardinal_tests crypto)

ADD_TEST(NAME cardinal_tests COMMAND cardinal_tests)
