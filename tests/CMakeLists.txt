cmake_minimum_required(VERSION 3.20)
project(cardinal_tests C)

# Host-side unit tests for the freestanding-but-host-compilable leaf code.
# Built with the HOST compiler (no cross flags) -- see scripts/run-tests.sh.

enable_testing()

SET(LIBS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../libs")
SET(COMMON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../common")

# Reuse the real crypto sources (the same ones sign_exec links).
ADD_SUBDIRECTORY("${LIBS_DIR}/crypto" ${CMAKE_CURRENT_BINARY_DIR}/crypto)

FILE(GLOB TEST_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/*.c)

# Compile the real kernel time.c into the suite. It pulls in the kernel type
# headers (via the ISA/PLATFORM defines the target build also passes) and its
# gmtime/strftime are renamed so they do not clash with the host libc. Scope the
# defines and the common/ include path to THIS source only, so they don't shadow
# the host headers the other tests rely on.
SET(TIME_SRC "${COMMON_DIR}/src/time.c")
SET_SOURCE_FILES_PROPERTIES("${TIME_SRC}" PROPERTIES
    COMPILE_DEFINITIONS "gmtime=cardinal_gmtime;strftime=cardinal_strftime;ISA_TYPES_H=${COMMON_DIR}/inc/platform/x86_64/types.h;PLATFORM_TYPES_H=${COMMON_DIR}/inc/platform/x86_64/pc/types.h"
    INCLUDE_DIRECTORIES "${COMMON_DIR}/inc")

ADD_EXECUTABLE(cardinal_tests "${TEST_SRCS}" "${TIME_SRC}")
TARGET_INCLUDE_DIRECTORIES(cardinal_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${LIBS_DIR}/crypto"
    "${CMAKE_CURRENT_SOURCE_DIR}/../servers/CoreNetwork/inc")
TARGET_LINK_LIBRARIES(cardinal_tests crypto)

ADD_TEST(NAME cardinal_tests COMMAND cardinal_tests)
