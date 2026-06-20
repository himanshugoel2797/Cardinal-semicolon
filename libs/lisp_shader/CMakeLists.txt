cmake_minimum_required(VERSION 3.20)
# Cardinal; shader tier (libs/lisp_shader) -- the typed/compiled/restricted Lisp
# sublanguage (see notes/core/lisp-shaders.md). A freestanding static library
# depending on `lisp` (it reuses the reader + lisp_value + lisp_bytes). The
# host-built test harness (test/) is separate; see test/build-and-run.sh -- it is
# NOT part of the target build. Added to the root build (S4): modules/SysLisp links
# this lib and exposes it as the (import sys-shader) capability -- compile + run
# typed shader kernels from within Lisp in-OS.
FILE(GLOB SHADER_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c)
ADD_LIBRARY(lisp_shader STATIC ${SHADER_SRCS})
TARGET_INCLUDE_DIRECTORIES(lisp_shader PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/inc")
TARGET_LINK_LIBRARIES(lisp_shader PUBLIC lisp)
TARGET_INCLUDE_DIRECTORIES(lisp_shader SYSTEM PUBLIC "${KERN_STDLIB_INCLUDE_DIR}")
# Mirror libs/lisp's flags: -fno-pic for the relocatable module model, scalar SSE
# re-enabled for hardware doubles, alignment-safe (see libs/lisp/CMakeLists.txt).
TARGET_COMPILE_OPTIONS(lisp_shader PRIVATE -fno-pic -msse -msse2 -mstackrealign
                                           -fno-vectorize -fno-slp-vectorize)
# The vector VM (sh_vm.c) uses SSE2 intrinsics via <immintrin.h>, which lives in
# clang's builtin resource dir -- hidden by -nostdinc (it implies -nobuiltininc).
# Re-expose ONLY that dir, placed AFTER common/inc on the search path so common's
# stdint/stddef still win; immintrin.h exists only here, so just the intrinsics
# resolve from it. AVX/SSE4 paths stay compiled out (no -mavx2/-msse4), so only the
# SSE2 intrinsics -- which the kernel already enables for the Lisp flonums -- run.
EXECUTE_PROCESS(COMMAND ${CMAKE_C_COMPILER} -print-resource-dir
                OUTPUT_VARIABLE CLANG_RESOURCE_DIR OUTPUT_STRIP_TRAILING_WHITESPACE)
IF(NOT CLANG_RESOURCE_DIR)
    MESSAGE(FATAL_ERROR "lisp_shader: could not determine the clang resource dir "
                        "(needed for <immintrin.h> under -nostdinc)")
ENDIF()
# Pass as a raw -isystem flag, NOT via TARGET_INCLUDE_DIRECTORIES: CMake filters out
# include dirs it considers implicit (the resource dir is one), which -nostdinc has
# actually suppressed -- so the flag form is the one that survives to the compiler.
TARGET_COMPILE_OPTIONS(lisp_shader PRIVATE "-isystem${CLANG_RESOURCE_DIR}/include")
