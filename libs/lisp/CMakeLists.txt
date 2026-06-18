cmake_minimum_required(VERSION 3.20)
# Kernel-resident Lisp core (Phase 0: values + reader/printer). Built as a
# freestanding static library against common/ for linking into a Sys*/Core*
# module later. The host-built test harness (test/) is separate; see
# build-and-run.sh -- it is NOT part of the target build.
FILE(GLOB LISP_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c)
ADD_LIBRARY(lisp STATIC ${LISP_SRCS})
TARGET_INCLUDE_DIRECTORIES(lisp PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/inc")
TARGET_INCLUDE_DIRECTORIES(lisp SYSTEM PUBLIC "${KERN_STDLIB_INCLUDE_DIR}")
# Pin -fno-pic explicitly (load-bearing for the relocatable module model), matching
# the other static libs rather than relying solely on inherited global flags.
SET_TARGET_PROPERTIES(lisp PROPERTIES COMPILE_OPTIONS "-fno-pic")
