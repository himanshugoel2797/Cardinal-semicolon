cmake_minimum_required(VERSION 3.20)
# Cardinal; shader tier (libs/lisp_shader) -- the typed/compiled/restricted Lisp
# sublanguage (see notes/core/lisp-shaders.md). A freestanding static library
# depending on `lisp` (it reuses the reader + lisp_value + lisp_bytes). The
# host-built test harness (test/) is separate; see test/build-and-run.sh -- it is
# NOT part of the target build. NOT yet added to libs/CMakeLists.txt: in-OS
# integration is a later phase (S4); for now this builds + tests host-side.
FILE(GLOB SHADER_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c)
ADD_LIBRARY(lisp_shader STATIC ${SHADER_SRCS})
TARGET_INCLUDE_DIRECTORIES(lisp_shader PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/inc")
TARGET_LINK_LIBRARIES(lisp_shader PUBLIC lisp)
TARGET_INCLUDE_DIRECTORIES(lisp_shader SYSTEM PUBLIC "${KERN_STDLIB_INCLUDE_DIR}")
# Mirror libs/lisp's flags: -fno-pic for the relocatable module model, scalar SSE
# re-enabled for hardware doubles, alignment-safe (see libs/lisp/CMakeLists.txt).
TARGET_COMPILE_OPTIONS(lisp_shader PRIVATE -fno-pic -msse -msse2 -mstackrealign
                                           -fno-vectorize -fno-slp-vectorize)
