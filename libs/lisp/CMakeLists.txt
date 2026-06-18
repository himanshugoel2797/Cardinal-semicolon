cmake_minimum_required(VERSION 3.20)
# Kernel-resident Lisp core (Phase 0: values + reader/printer). Built as a
# freestanding static library against common/ for linking into a Sys*/Core*
# module later. The host-built test harness (test/) is separate; see
# build-and-run.sh -- it is NOT part of the target build.
FILE(GLOB LISP_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c)
ADD_LIBRARY(lisp STATIC ${LISP_SRCS})
TARGET_INCLUDE_DIRECTORIES(lisp PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/inc")
TARGET_INCLUDE_DIRECTORIES(lisp SYSTEM PUBLIC "${KERN_STDLIB_INCLUDE_DIR}")
# -fno-pic is load-bearing for the relocatable module model. SSE is re-enabled
# here (overriding the kernel's blanket -mno-sse) so the Scheme runtime can use
# native hardware doubles for flonums: it runs only in task context, where
# SysTaskMgr saves/restores FP state per context switch. See
# notes/core/lisp-substrate.md ("Floating point is in scope").
TARGET_COMPILE_OPTIONS(lisp PRIVATE -fno-pic -msse -msse2)
