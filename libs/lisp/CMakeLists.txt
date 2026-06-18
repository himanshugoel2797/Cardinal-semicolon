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
#
# -mstackrealign: the kernel/module call path is -mno-sse and does not guarantee
# the 16-byte stack alignment the SysV ABI assumes, so SSE code's aligned spills
# (movaps) would #GP on a stack that is only 8-aligned on entry. This makes each
# SSE function realign its own stack -- the standard fix for SSE in a kernel that
# is otherwise built without it.
#
# -fno-vectorize -fno-slp-vectorize: the flonum path is SCALAR double arithmetic
# (movsd/addsd, which tolerate unaligned operands). Auto-vectorization turns it
# into PACKED ops (subpd) that load 16-byte constants from .rodata with an ALIGNED
# move (movapd) -- and the kernel module loader does not 16-align section data, so
# that aligned load #GPs. Disabling vectorization keeps the runtime scalar (its
# intent anyway) and alignment-safe.
TARGET_COMPILE_OPTIONS(lisp PRIVATE -fno-pic -msse -msse2 -mstackrealign
                                    -fno-vectorize -fno-slp-vectorize)
