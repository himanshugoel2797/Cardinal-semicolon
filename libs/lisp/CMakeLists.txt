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
# Auto-vectorization is now SAFE (and on): it emits PACKED ops (subpd) that load
# 16-byte constants from .rodata with an ALIGNED move (movapd), which formerly #GP'd
# because the module loader didn't align section data. The loader now lands a module
# at a 64-aligned base (load_script.c) and aligns .bss to sh_addralign (elf.c), so the
# linker's sh_addralign-aligned .rodata.cst16 / packed constants are aligned at
# runtime. Auto-vectorized loops over runtime heap pointers use UNALIGNED moves
# (movups) anyway, so only those aligned constant loads needed the fix. Kept at SSE2
# width (no AVX): some targets (Cherry Trail / Airmont) lack AVX.
TARGET_COMPILE_OPTIONS(lisp PRIVATE -fno-pic -msse -msse2 -mstackrealign)

# Direct-threaded VM dispatch (lbc_vm_exec): a per-chunk table of computed-goto
# handler labels instead of a switch. It only pays off once the surrounding loop
# is optimized (~+11% on hot tail loops at -O2; ~0 at -O0, where the un-optimized
# safe-point prologue dominates), so enable it for optimized (Release) builds and
# keep the readable switch for Debug. The two paths are differentially fuzz-equal.
TARGET_COMPILE_DEFINITIONS(lisp PRIVATE $<$<CONFIG:Release>:LBC_THREADED>)
